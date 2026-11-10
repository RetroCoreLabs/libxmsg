/*
 * xrout_walk.c - walk a host's routing table with XSGSY (see xrout_walk.h).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/xrout/xrout_walk.h"

#include <string.h>

#include "libxmsg/log/xmsg_log.h"
#include "libxmsg/sintran/sintran_hdr.h"
#include "libxmsg/xrout/xrout_letter.h"

#define WALK_DEFAULT_TIMEOUT_MS 10000u

/* End the walk: free the session slot first, so on_done may start another one. */
static void walk_finish(XroutWalk *w, int result)
{
    XroutWalkCallbacks cb = w->cb;
    unsigned entries = w->entries;
    w->state = XROUT_WALK_IDLE;
    w->base.wants_tx = false;
    if (w->base.node != NULL)
    {
        (void)xmsg_node_detach(w->base.node, &w->base);
    }
    if (cb.on_done != NULL)
    {
        cb.on_done(cb.ctx, result, entries);
    }
}

/* The next question is due. */
static void walk_due(XroutWalk *w, uint16_t asked, uint32_t now_ms)
{
    w->asked = asked;
    w->state = XROUT_WALK_SEND;
    w->since_ms = now_ms;
    w->base.wants_tx = true;
}

static int walk_poll_tx(XmsgSession *s, uint32_t now_ms)
{
    XroutWalk *w = (XroutWalk *)s;
    uint8_t *body;
    size_t cap;
    XmsgSubhdr sub;
    int rc;

    if (w->state != XROUT_WALK_SEND)
    {
        s->wants_tx = false;
        return 0;
    }
    if (xmsg_node_send_begin(s->node, &body, &cap) != XMSG_OK)
    {
        return 0;
    }
    xrout_route_request_write(body, w->asked);
    sub.frame_flags = XROUT_ROUTE_REQ_FRAME_FLAGS;
    sub.role = XROUT_ROUTE_REQ_ROLE;
    sub.dst_system = w->host_system;
    sub.dst_port = 0; /* the host's XROUT */
    sub.src_system = 0; /* filled in by the node */
    sub.src_port = s->local_port;
    sub.xmcsm = XROUT_ROUTE_XMCSM;
    rc = xmsg_node_send_commit(s->node, s->peer_node, &sub, XROUT_ROUTE_REQ_SIZE, now_ms,
                               &w->sent_flags1);
    if (rc != XMSG_OK)
    {
        xmsg_node_send_cancel(s->node);
        return 0; /* EAGAIN: next free slot; the timeout covers a slot that never comes */
    }
    w->state = XROUT_WALK_WAIT;
    w->since_ms = now_ms;
    w->steps++;
    s->wants_tx = false;
    XMSG_LOG(XMSG_LOG_XROUT, XMSG_LOG_DEBUG, "routing walk: asked node %u for system %u, F1=0x%04X",
             s->peer_node, w->asked, w->sent_flags1);
    return 1;
}

static void walk_on_datagram(XmsgSession *s, const XmsgDatagram *dg, uint32_t now_ms)
{
    XroutWalk *w = (XroutWalk *)s;
    XroutRouteEntry e;
    uint8_t status = 0;
    int rc;

    if (w->state != XROUT_WALK_WAIT || dg->repeat)
    {
        return; /* nothing asked, or the host sent a reply again that we already took */
    }
    rc = xrout_route_reply_read(dg->body, dg->body_len, &status, &e);
    if (rc != XMSG_OK)
    {
        w->last_status = status;
        XMSG_LOG(XMSG_LOG_XROUT, XMSG_LOG_WARN,
                 "routing walk: node %u answered system %u with status %u (%s), %u bytes",
                 s->peer_node, w->asked, status, xrout_error_name(status),
                 (unsigned)dg->body_len);
        walk_finish(w, XMSG_EPROTO);
        return;
    }
    if (e.system == 0)
    {
        walk_finish(w, XMSG_OK); /* "no entry at or above": the end of the table */
        return;
    }
    if (e.system < w->asked)
    {
        /* Below what was asked: a late answer to an earlier question, never the current one. */
        XMSG_LOG(XMSG_LOG_XROUT, XMSG_LOG_DEBUG,
                 "routing walk: ignored a reply for system %u while asking for %u", e.system,
                 w->asked);
        return;
    }
    w->entries++;
    if (w->cb.on_entry != NULL)
    {
        w->cb.on_entry(w->cb.ctx, &e);
    }
    if (e.system == 0xFFFFu)
    {
        walk_finish(w, XMSG_OK); /* there is nothing above it to ask for */
        return;
    }
    if (w->steps >= XROUT_WALK_MAX_STEPS)
    {
        XMSG_LOG(XMSG_LOG_XROUT, XMSG_LOG_WARN, "routing walk: no end after %u questions",
                 w->steps);
        walk_finish(w, XMSG_EPROTO);
        return;
    }
    w->resends = 0;
    walk_due(w, (uint16_t)(e.system + 1u), now_ms);
}

static void walk_tick(XmsgSession *s, uint32_t now_ms)
{
    XroutWalk *w = (XroutWalk *)s;
    if (w->state == XROUT_WALK_IDLE)
    {
        return;
    }
    if ((uint32_t)(now_ms - w->since_ms) >= w->timeout_ms)
    {
        XMSG_LOG(XMSG_LOG_XROUT, XMSG_LOG_WARN,
                 "routing walk: node %u gave no answer for system %u in %u ms", s->peer_node,
                 w->asked, (unsigned)w->timeout_ms);
        walk_finish(w, XMSG_ETIMEDOUT);
    }
}

/* Ask the same question again, a bounded number of times. */
static void walk_resend(XroutWalk *w, const char *why)
{
    if (w->resends >= XROUT_WALK_MAX_RESENDS)
    {
        XMSG_LOG(XMSG_LOG_XROUT, XMSG_LOG_WARN, "routing walk: %s %u times; giving up", why,
                 (unsigned)w->resends);
        walk_finish(w, XMSG_EPROTO);
        return;
    }
    w->resends++;
    /* The timeout clock keeps running from the first send: a resend buys no extra time. */
    w->state = XROUT_WALK_SEND;
    w->base.wants_tx = true;
}

static void walk_on_error(XmsgSession *s, int16_t code, uint16_t flags1)
{
    XroutWalk *w = (XroutWalk *)s;
    if (w->state != XROUT_WALK_WAIT || flags1 != w->sent_flags1)
    {
        return; /* about another session's frame on the same peer */
    }
    w->last_error = code;
    if (code == XMSG_XE_XENSE)
    {
        /* The node has already stepped our count down (xmsg_flags1_on_xense); the question
         * goes again at the corrected number. */
        walk_resend(w, "XENSE");
        return;
    }
    XMSG_LOG(XMSG_LOG_XROUT, XMSG_LOG_WARN, "routing walk: node %u refused F1=0x%04X with %d",
             s->peer_node, flags1, (int)code);
    walk_finish(w, XMSG_EPROTO);
}

static void walk_on_peer_restart(XmsgSession *s)
{
    XroutWalk *w = (XroutWalk *)s;
    if (w->state == XROUT_WALK_WAIT)
    {
        walk_resend(w, "peer restarted"); /* it dropped the question with its old count */
    }
}

static const XmsgSessionOps WALK_OPS = {walk_on_datagram, walk_poll_tx,  walk_tick,
                                        NULL,             walk_on_error, walk_on_peer_restart};

int xrout_walk_start(XroutWalk *w, XmsgNode *n, uint16_t host_node, uint16_t host_system,
                     uint32_t timeout_ms, const XroutWalkCallbacks *cb, uint32_t now_ms)
{
    int rc;
    if (w == NULL || n == NULL || cb == NULL)
    {
        return XMSG_EINVAL;
    }
    if (w->state != XROUT_WALK_IDLE && w->base.attached)
    {
        return XMSG_EAGAIN;
    }
    if (!xmsg_node_is_ready(n))
    {
        return XMSG_ESTATE;
    }
    memset(w, 0, sizeof *w);
    w->base.ops = &WALK_OPS;
    w->base.peer_node = host_node;
    /* The reply's source address is OUR system and port (the host echoes the asker in both
     * fields), so accept any system and any port, and learn none. */
    w->base.peer_system = 0;
    w->base.peer_port = 0;
    w->base.any_peer_port = true;
    w->cb = *cb;
    w->host_system = host_system;
    w->timeout_ms = (timeout_ms == 0) ? WALK_DEFAULT_TIMEOUT_MS : timeout_ms;
    rc = xmsg_node_attach(n, &w->base);
    if (rc != XMSG_OK)
    {
        return rc;
    }
    walk_due(w, 1, now_ms); /* system 0 means "none", so the first real number is 1 */
    return XMSG_OK;
}

int xrout_walk_cancel(XroutWalk *w)
{
    if (w == NULL || w->state == XROUT_WALK_IDLE)
    {
        return XMSG_ESTATE;
    }
    walk_finish(w, XMSG_ESTATE);
    return XMSG_OK;
}

bool xrout_walk_is_busy(const XroutWalk *w)
{
    return w != NULL && w->state != XROUT_WALK_IDLE;
}
