/*
 * xrout_names_walk.c - read a remote XROUT's name table with XSGNI (see xrout_names_walk.h).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/xrout/xrout_names_walk.h"

#include <string.h>

#include "libxmsg/log/xmsg_log.h"
#include "libxmsg/sintran/sintran_hdr.h"
#include "libxmsg/xrout/xrout_letter.h"

#define NAMES_WALK_DEFAULT_TIMEOUT_MS 10000u

/* End the walk: free the session slot first, so on_done may start another one. */
static void names_finish(XroutNamesWalk *w, int result)
{
    XroutNamesWalkCallbacks cb = w->cb;
    unsigned rows = w->rows;
    w->state = XROUT_NAMES_WALK_IDLE;
    w->base.wants_tx = false;
    if (w->base.node != NULL)
    {
        (void)xmsg_node_detach(w->base.node, &w->base);
    }
    if (cb.on_done != NULL)
    {
        cb.on_done(cb.ctx, result, rows);
    }
}

/* The next question is due. */
static void names_due(XroutNamesWalk *w, uint32_t asked, uint32_t now_ms)
{
    w->asked = asked;
    w->state = XROUT_NAMES_WALK_SEND;
    w->since_ms = now_ms;
    w->base.wants_tx = true;
}

static int names_poll_tx(XmsgSession *s, uint32_t now_ms)
{
    XroutNamesWalk *w = (XroutNamesWalk *)s;
    uint8_t *body;
    size_t cap;
    size_t len;
    XmsgSubhdr sub;
    int rc;

    if (w->state != XROUT_NAMES_WALK_SEND)
    {
        s->wants_tx = false;
        return 0;
    }
    if (xmsg_node_send_begin(s->node, &body, &cap) != XMSG_OK)
    {
        return 0;
    }
    if (cap < XROUT_NAME_REQ_MAX)
    {
        xmsg_node_send_cancel(s->node);
        return 0;
    }
    len = xrout_name_request_write(body, w->asked);
    sub.frame_flags = XROUT_NAME_REQ_FRAME_FLAGS;
    sub.role = XROUT_NAME_REQ_ROLE;
    sub.dst_system = w->remote_system;
    sub.dst_port = 0;   /* the remote XROUT */
    sub.src_system = 0; /* filled in by the node */
    sub.src_port = s->local_port;
    sub.xmcsm = XROUT_NAME_XMCSM;
    rc = xmsg_node_send_commit(s->node, s->peer_node, &sub, len, now_ms, &w->sent_flags1);
    if (rc != XMSG_OK)
    {
        xmsg_node_send_cancel(s->node);
        return 0; /* EAGAIN: next free slot; the timeout covers a slot that never comes */
    }
    w->state = XROUT_NAMES_WALK_WAIT;
    w->since_ms = now_ms;
    w->steps++;
    s->wants_tx = false;
    XMSG_LOG(XMSG_LOG_XROUT, XMSG_LOG_DEBUG,
             "name walk: asked system %u for the row at or above 0x%08lX, F1=0x%04X",
             w->remote_system, (unsigned long)w->asked, w->sent_flags1);
    return 1;
}

static void names_on_datagram(XmsgSession *s, const XmsgDatagram *dg, uint32_t now_ms)
{
    XroutNamesWalk *w = (XroutNamesWalk *)s;
    XroutNameRow row;
    uint8_t status = 0;
    bool is_end = false;
    uint32_t magic;
    int rc;

    if (w->state != XROUT_NAMES_WALK_WAIT || dg->repeat)
    {
        return; /* nothing asked, or the remote sent a reply again that we already took */
    }
    rc = xrout_name_reply_read(dg->body, dg->body_len, &status, &row, &is_end);
    if (rc != XMSG_OK)
    {
        w->last_status = status;
        XMSG_LOG(XMSG_LOG_XROUT, XMSG_LOG_WARN,
                 "name walk: system %u answered 0x%08lX with status %u (%s), %u bytes",
                 w->remote_system, (unsigned long)w->asked, status, xrout_error_name(status),
                 (unsigned)dg->body_len);
        names_finish(w, XMSG_EPROTO);
        return;
    }
    if (is_end)
    {
        names_finish(w, XMSG_OK); /* "no row at or above": the end of the table */
        return;
    }
    magic = xrout_name_magic(&row);
    if (magic < w->asked)
    {
        /* Below what was asked: a late answer to an earlier question, never the current one. */
        XMSG_LOG(XMSG_LOG_XROUT, XMSG_LOG_DEBUG,
                 "name walk: ignored a reply for 0x%08lX while asking for 0x%08lX",
                 (unsigned long)magic, (unsigned long)w->asked);
        return;
    }
    w->rows++;
    if (w->cb.on_row != NULL)
    {
        w->cb.on_row(w->cb.ctx, &row);
    }
    if (magic == 0xFFFFFFFFu)
    {
        names_finish(w, XMSG_OK); /* there is nothing above it to ask for */
        return;
    }
    if (w->steps >= XROUT_NAMES_WALK_MAX_STEPS)
    {
        XMSG_LOG(XMSG_LOG_XROUT, XMSG_LOG_WARN, "name walk: no end after %u questions", w->steps);
        names_finish(w, XMSG_EPROTO);
        return;
    }
    w->resends = 0;
    names_due(w, magic + 1u, now_ms);
}

static void names_tick(XmsgSession *s, uint32_t now_ms)
{
    XroutNamesWalk *w = (XroutNamesWalk *)s;
    if (w->state == XROUT_NAMES_WALK_IDLE)
    {
        return;
    }
    if ((uint32_t)(now_ms - w->since_ms) >= w->timeout_ms)
    {
        XMSG_LOG(XMSG_LOG_XROUT, XMSG_LOG_WARN,
                 "name walk: system %u gave no answer for 0x%08lX in %u ms", w->remote_system,
                 (unsigned long)w->asked, (unsigned)w->timeout_ms);
        names_finish(w, XMSG_ETIMEDOUT);
    }
}

/* Ask the same question again, a bounded number of times. */
static void names_resend(XroutNamesWalk *w, const char *why)
{
    if (w->resends >= XROUT_NAMES_WALK_MAX_RESENDS)
    {
        XMSG_LOG(XMSG_LOG_XROUT, XMSG_LOG_WARN, "name walk: %s %u times; giving up", why,
                 (unsigned)w->resends);
        names_finish(w, XMSG_EPROTO);
        return;
    }
    w->resends++;
    /* The timeout clock keeps running from the first send: a resend buys no extra time. */
    w->state = XROUT_NAMES_WALK_SEND;
    w->base.wants_tx = true;
}

static void names_on_error(XmsgSession *s, int16_t code, uint16_t flags1)
{
    XroutNamesWalk *w = (XroutNamesWalk *)s;
    if (w->state != XROUT_NAMES_WALK_WAIT || flags1 != w->sent_flags1)
    {
        return; /* about another session's frame on the same peer */
    }
    w->last_error = code;
    if (code == XMSG_XE_XENSE)
    {
        /* The node has already stepped our count down (xmsg_flags1_on_xense); the question
         * goes again at the corrected number. */
        names_resend(w, "XENSE");
        return;
    }
    XMSG_LOG(XMSG_LOG_XROUT, XMSG_LOG_WARN, "name walk: node %u refused F1=0x%04X with %d",
             s->peer_node, flags1, (int)code);
    names_finish(w, XMSG_EPROTO);
}

static void names_on_peer_restart(XmsgSession *s)
{
    XroutNamesWalk *w = (XroutNamesWalk *)s;
    if (w->state == XROUT_NAMES_WALK_WAIT)
    {
        names_resend(w, "peer restarted"); /* it dropped the question with its old count */
    }
}

static const XmsgSessionOps NAMES_WALK_OPS = {
    names_on_datagram, names_poll_tx, names_tick, NULL, names_on_error, names_on_peer_restart};

int xrout_names_walk_start(XroutNamesWalk *w, XmsgNode *n, uint16_t link_node,
                           uint16_t remote_system, uint32_t timeout_ms,
                           const XroutNamesWalkCallbacks *cb, uint32_t now_ms)
{
    int rc;
    if (w == NULL || n == NULL || cb == NULL)
    {
        return XMSG_EINVAL;
    }
    if (w->state != XROUT_NAMES_WALK_IDLE && w->base.attached)
    {
        return XMSG_EAGAIN;
    }
    if (!xmsg_node_is_ready(n))
    {
        return XMSG_ESTATE;
    }
    memset(w, 0, sizeof *w);
    w->base.ops = &NAMES_WALK_OPS;
    w->base.peer_node = link_node;
    /* The reply's source address is OUR system and port (XROUT echoes the asker in both
     * fields), so accept any system and any port, and learn none. */
    w->base.peer_system = 0;
    w->base.peer_port = 0;
    w->base.any_peer_port = true;
    w->cb = *cb;
    w->remote_system = remote_system;
    w->timeout_ms = (timeout_ms == 0) ? NAMES_WALK_DEFAULT_TIMEOUT_MS : timeout_ms;
    rc = xmsg_node_attach(n, &w->base);
    if (rc != XMSG_OK)
    {
        return rc;
    }
    names_due(w, 0, now_ms); /* the captured first call asks from magic 0 */
    return XMSG_OK;
}

int xrout_names_walk_cancel(XroutNamesWalk *w)
{
    if (w == NULL || w->state == XROUT_NAMES_WALK_IDLE)
    {
        return XMSG_ESTATE;
    }
    names_finish(w, XMSG_ESTATE);
    return XMSG_OK;
}

bool xrout_names_walk_is_busy(const XroutNamesWalk *w)
{
    return w != NULL && w->state != XROUT_NAMES_WALK_IDLE;
}
