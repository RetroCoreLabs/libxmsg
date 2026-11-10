/*
 * xmsg_flags1.c - per-peer outgoing datagram numbers.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/xmsg/xmsg_flags1.h"

#include <string.h>

#include "libxmsg/log/xmsg_log.h"

void xmsg_flags1_init(XmsgFlags1Table *t, const XmsgFlags1Port *port, uint8_t persist_every)
{
    memset(t, 0, sizeof *t);
    if (port != NULL)
    {
        t->port = *port;
    }
    t->persist_every = (persist_every == 0) ? 1 : persist_every;
}

static bool store_usable(const XmsgFlags1Table *t)
{
    return t->port.load != NULL && t->port.save != NULL;
}

static void save_bound(XmsgFlags1Table *t, XmsgFlags1Entry *e, uint16_t bound)
{
    e->saved_bound = bound;
    if (store_usable(t))
    {
        if (t->port.save(t->port.ctx, e->node, bound) != 0)
        {
            XMSG_LOG(XMSG_LOG_FLAGS1, XMSG_LOG_ERROR, "node %u: saving Flags1 bound 0x%04X failed",
                     e->node, bound);
        }
    }
}

/* Find or create the entry for a peer, loading its stored value on first sight. */
static XmsgFlags1Entry *entry_for(XmsgFlags1Table *t, uint16_t node, bool create)
{
    XmsgFlags1Entry *free_slot = NULL;
    for (int i = 0; i < XMSG_MAX_PEERS; i++)
    {
        XmsgFlags1Entry *e = &t->entries[i];
        if (e->in_use && e->node == node)
        {
            return e;
        }
        if (!e->in_use && free_slot == NULL)
        {
            free_slot = e;
        }
    }
    if (!create || free_slot == NULL)
    {
        return NULL;
    }
    free_slot->in_use = true;
    free_slot->node = node;
    free_slot->next = 0;
    free_slot->xense_steps = 0;
    if (store_usable(t))
    {
        uint16_t stored = 0;
        if (t->port.load(t->port.ctx, node, &stored) == 0)
        {
            /* The stored value is the bound: resume there, at worst a few ahead. */
            free_slot->next = stored;
            XMSG_LOG(XMSG_LOG_FLAGS1, XMSG_LOG_INFO,
                     "node %u: resuming Flags1 at 0x%04X from the store", node, stored);
        }
        else
        {
            XMSG_LOG(XMSG_LOG_FLAGS1, XMSG_LOG_INFO, "node %u: nothing stored, Flags1 starts at 0",
                     node);
        }
    }
    else if (!t->store_missing_warned)
    {
        t->store_missing_warned = true;
        XMSG_LOG(XMSG_LOG_FLAGS1, XMSG_LOG_WARN,
                 "no Flags1 store: counters start at 0 and are lost at restart (the peer will drop "
                 "or refuse frames until they converge)");
    }
    free_slot->saved_bound = free_slot->next;
    return free_slot;
}

int xmsg_flags1_allocate(XmsgFlags1Table *t, uint16_t node, uint16_t *out)
{
    XmsgFlags1Entry *e = entry_for(t, node, true);
    if (e == NULL)
    {
        XMSG_LOG(XMSG_LOG_FLAGS1, XMSG_LOG_ERROR,
                 "no room for a Flags1 counter for node %u (%d peers)", node, XMSG_MAX_PEERS);
        return XMSG_ENOSPC;
    }
    if (e->next == 0xFFFFu)
    {
        e->next = 0; /* 0xFFFF means "reachability" on the wire and is never a datagram number */
    }
    /* Persist-at-send with a bound: before handing out f, the store must already promise at
     * least f + 1, so a restart can never resume BEHIND f + 1 (behind is silent loss; ahead is
     * recoverable). With persist_every = 1 this saves f + 1 on every frame, as the C# does. */
    if ((int32_t)(int16_t)(e->saved_bound - e->next) <= 0)
    {
        save_bound(t, e, (uint16_t)(e->next + t->persist_every));
    }
    *out = e->next;
    e->next++;
    XMSG_LOG(XMSG_LOG_FLAGS1, XMSG_LOG_TRACE, "node %u: ORIGINATE at Flags1 0x%04X (next 0x%04X)",
             node, *out, e->next);
    return XMSG_OK;
}

uint16_t xmsg_flags1_peek(XmsgFlags1Table *t, uint16_t node)
{
    XmsgFlags1Entry *e = entry_for(t, node, true);
    return (e != NULL) ? e->next : 0;
}

void xmsg_flags1_reset(XmsgFlags1Table *t, uint16_t node)
{
    XmsgFlags1Entry *e = entry_for(t, node, true);
    if (e == NULL)
    {
        return;
    }
    XMSG_LOG(XMSG_LOG_FLAGS1, XMSG_LOG_INFO,
             "node %u: Flags1 reset 0x%04X -> 0 (reachability handshake)", node, e->next);
    e->next = 0;
    e->xense_steps = 0;
    save_bound(t, e, 0);
}

bool xmsg_flags1_on_xense(XmsgFlags1Table *t, uint16_t node, uint16_t refused)
{
    XmsgFlags1Entry *e = entry_for(t, node, false);
    if (e == NULL)
    {
        return false;
    }
    /* Only a number we sent at the current count can be stepped from: one of 1..0x7FFF below
     * `next` (serial-number distance, so a wrap past 0xFFFF still works). Anything else was sent
     * before a reachability reset or before an earlier step, and the peer refusing it says
     * nothing about the count now. Live, XMSGGW 2026-11-10 17:18:31: a frame went at 0x0044, the
     * reset came, the frame went again at 0x0000 (accepted), and then D100's refusal of the old
     * 0x0044 arrived; stepping from it set the count to 0x0043 and every later frame was refused. */
    uint16_t distance = (uint16_t)(e->next - refused);
    if (distance == 0u || distance >= 0x8000u)
    {
        XMSG_LOG(XMSG_LOG_FLAGS1, XMSG_LOG_INFO,
                 "node %u: XENSE at Flags1 0x%04X is for a frame sent before the count became "
                 "0x%04X; ignored",
                 node, refused, e->next);
        return false;
    }
    e->xense_steps++;
    if (e->xense_steps > XMSG_FLAGS1_MAX_XENSE_STEPS || refused == 0)
    {
        XMSG_LOG(XMSG_LOG_FLAGS1, XMSG_LOG_ERROR,
                 "node %u: giving up after %u XENSE step(s), last refused 0x%04X; the peer's "
                 "expectation of us is not converging (check LIST-SYSTEMS on the machine)",
                 node, e->xense_steps, refused);
        return false;
    }
    e->next = (uint16_t)(refused - 1);
    XMSG_LOG(XMSG_LOG_FLAGS1, XMSG_LOG_WARN,
             "node %u: XENSE at Flags1 0x%04X - we are ahead; next becomes 0x%04X", node, refused,
             e->next);
    save_bound(t, e, (uint16_t)(e->next + t->persist_every));
    return true;
}

void xmsg_flags1_on_delivered(XmsgFlags1Table *t, uint16_t node)
{
    XmsgFlags1Entry *e = entry_for(t, node, false);
    if (e != NULL)
    {
        e->xense_steps = 0;
    }
}
