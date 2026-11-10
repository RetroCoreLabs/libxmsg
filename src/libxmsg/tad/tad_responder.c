/*
 * tad_responder.c - a terminal session from a SINTRAN machine: the side that answers.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * The frame shapes and the reasons for them are in tad_responder.h.
 */
#include "libxmsg/tad/tad_responder.h"

#include <string.h>

#include "libxmsg/log/xmsg_log.h"
#include "libxmsg/xmsg_endian.h"
#include "libxmsg/xrout/xrout_letter.h"

/** The kinds of frame the responder sends. */
typedef enum TadResponderSend
{
    RS_NONE = 0,
    RS_ACCEPT,      /**< the XROUT reply to the letter */
    RS_PORT_ASSIGN, /**< the answer to the session setup */
    RS_DUMM,        /**< the priming DUMM from the session port */
    RS_ESRS,        /**< escape answered */
    RS_EDRS,        /**< escape refused (disabled) */
    RS_RESE_1,      /**< the bring-up RESE after the ESRS */
    RS_RESE_2,      /**< the bring-up RESE answering the first RECO */
    RS_CHAIN,       /**< the oldest closed chain */
    RS_NOTICE,      /**< 0xFD from *TADADM */
    RS_DCON         /**< our hang-up */
} TadResponderSend;

/* ------------------------------------------------------------------------------------------ */
/* State and queues                                                                           */
/* ------------------------------------------------------------------------------------------ */

static TadResponderChain *open_chain(TadResponder *r)
{
    if (r->c_closed >= TAD_RESP_CHAINS)
    {
        return NULL;
    }
    return &r->chains[(r->c_head + r->c_closed) % TAD_RESP_CHAINS];
}

static bool text_may_go(const TadResponder *r)
{
    return r->state == TAD_RESP_RUNNING || r->state == TAD_RESP_CLOSING;
}

static bool open_chain_waits(TadResponder *r)
{
    TadResponderChain *c = open_chain(r);

    return c != NULL && c->len > 0 && text_may_go(r);
}

/*
 * What goes next. Control frames first, in the order they were queued, except the 0xFD notice
 * and our DCON, which wait until every chain is out. A chain goes only once the session runs,
 * and waits while the asker owes TAD_RESP_OUTPUT_WINDOW answers. RS_NONE: nothing may go now.
 */
static TadResponderSend next_kind(TadResponder *r)
{
    TadResponderSend head = RS_NONE;
    bool chains = (r->c_closed > 0) || open_chain_waits(r);

    if (r->q_count > 0)
    {
        head = (TadResponderSend)r->queue[r->q_head];
        if (head != RS_NOTICE && head != RS_DCON)
        {
            return head;
        }
    }
    if (chains && text_may_go(r))
    {
        if (r->answers_owed >= TAD_RESP_OUTPUT_WINDOW)
        {
            return RS_NONE;
        }
        return RS_CHAIN;
    }
    if (chains)
    {
        return RS_NONE; /* text waits for the end of the bring-up */
    }
    return head;
}

static void update_wants_tx(TadResponder *r)
{
    r->adm.base.wants_tx = (r->inflight == RS_NONE) && (next_kind(r) != RS_NONE);
}

static void detach_ports(TadResponder *r)
{
    if (r->adm.base.attached && r->adm.base.node != NULL)
    {
        (void)xmsg_node_detach(r->adm.base.node, &r->adm.base); /* safe inside node callbacks */
    }
    if (r->term.base.attached && r->term.base.node != NULL)
    {
        (void)xmsg_node_detach(r->term.base.node, &r->term.base);
    }
}

static void drop_chains(TadResponder *r)
{
    r->c_closed = 0;
    r->chains[r->c_head].len = 0;
    r->chains[r->c_head].bdat_at = -1;
}

static void set_state(TadResponder *r, TadResponderState st, int reason, uint32_t now_ms)
{
    if (r->state == st)
    {
        return;
    }
    XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_INFO, "responder %s -> %s (%d)",
             tad_responder_state_name(r->state), tad_responder_state_name(st), reason);
    r->state = st;
    r->state_since_ms = now_ms;
    if (st == TAD_RESP_CLOSED || st == TAD_RESP_FAILED)
    {
        r->q_count = 0;
        drop_chains(r);
        r->inflight = RS_NONE;
        r->input_granted = false;
        r->adm.base.wants_tx = false;
        detach_ports(r);
    }
    if (r->cb.state != NULL)
    {
        r->cb.state(r->cb.ctx, st, reason);
    }
}

static void fail(TadResponder *r, int error, uint32_t now_ms)
{
    set_state(r, TAD_RESP_FAILED, error, now_ms);
}

static void enqueue(TadResponder *r, TadResponderSend kind, uint32_t now_ms)
{
    if (r->q_count >= TAD_RESP_QUEUE)
    {
        XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_ERROR,
                 "responder queue full (%u): the asker sent faster than we answer",
                 (unsigned)TAD_RESP_QUEUE);
        fail(r, XMSG_ENOSPC, now_ms);
        return;
    }
    r->queue[(r->q_head + r->q_count) % TAD_RESP_QUEUE] = (uint8_t)kind;
    r->q_count++;
    update_wants_tx(r);
}

/* Close the open chain: it goes out as one frame. XMSG_EAGAIN when no chain is free after it. */
static int close_chain(TadResponder *r)
{
    TadResponderChain *c = open_chain(r);
    TadResponderChain *next;

    if (c == NULL)
    {
        return XMSG_EAGAIN;
    }
    if (c->len == 0)
    {
        return XMSG_OK;
    }
    c->bdat_at = -1;
    r->c_closed++;
    next = open_chain(r);
    if (next != NULL)
    {
        next->len = 0;
        next->bdat_at = -1;
    }
    update_wants_tx(r);
    return XMSG_OK;
}

/* Room for `need` more bytes in the open chain; closes it and opens the next when it is full. */
static TadResponderChain *chain_with_room(TadResponder *r, size_t need)
{
    TadResponderChain *c = open_chain(r);

    if (c == NULL)
    {
        return NULL;
    }
    if ((size_t)c->len + need <= TAD_RESP_CHAIN_MAX)
    {
        return c;
    }
    if (close_chain(r) != XMSG_OK)
    {
        return NULL;
    }
    return open_chain(r);
}

/* ------------------------------------------------------------------------------------------ */
/* Building frames                                                                            */
/* ------------------------------------------------------------------------------------------ */

/* The port assignment chain, laid out as the real D102 sends it (record 296):
 *   00 | 07 05 00 <system> <session port> | 1F 03 4C 00 00 | 00 | 0B 02 <unit> | 15 02 01 08 | FF 00
 * The 1F, 15 and FF data are copied as observed (see the header: NOT decoded). */
static size_t build_port_assign(const TadResponder *r, uint8_t *chain, size_t cap)
{
    TadWriter w;
    uint8_t d07[5];
    static const uint8_t d1f[3] = {0x4C, 0x00, 0x00};
    static const uint8_t d15[2] = {0x01, 0x08};
    uint8_t d0b[2];

    d07[0] = 0x00;
    xmsg_be16_write(d07 + 1, xmsg_node_system(r->adm.base.node));
    xmsg_be16_write(d07 + 3, r->term.base.local_port);
    xmsg_be16_write(d0b, r->cfg.logical_unit);
    tad_writer_init(&w, chain, cap);
    (void)tad_writer_add_host(&w, (uint8_t)TAD_OP_RAW_07, d07, sizeof d07);
    (void)tad_writer_add_host(&w, (uint8_t)TAD_OP_OPSV, d1f, sizeof d1f);
    (void)tad_writer_add_host(&w, (uint8_t)TAD_OP_LUN, d0b, sizeof d0b);
    (void)tad_writer_add_host(&w, (uint8_t)TAD_OP_FBSI, d15, sizeof d15);
    (void)tad_writer_add_host(&w, (uint8_t)TAD_OP_RAW_FF, NULL, 0);
    return tad_writer_length(&w);
}

/* One message with no data, as a whole chain. */
static size_t build_bare(uint8_t opcode, uint8_t *chain, size_t cap)
{
    TadWriter w;

    tad_writer_init(&w, chain, cap);
    (void)tad_writer_add_host(&w, opcode, NULL, 0);
    return tad_writer_length(&w);
}

/* What a chain holds, for the frame flags and for whether the asker will answer it. */
typedef struct ChainFacts
{
    bool echo_off; /**< ECKM 0xFF: the frame goes as 0x92 (see the header) */
    bool owes;     /**< the asker answers it with DUMM or CERS (tad_asker.h reply rule) */
} ChainFacts;

static ChainFacts chain_facts(const uint8_t *chain, size_t len)
{
    ChainFacts f = {false, false};
    TadReader rd;
    TadMessage m;
    bool content = false;
    bool rfi = false;

    tad_reader_init(&rd, chain, len);
    while (tad_reader_next(&rd, &m))
    {
        switch (m.opcode)
        {
        case TAD_OP_RFI:
            rfi = true;
            break;
        case TAD_OP_DUMM:
        case TAD_OP_ESRS:
            break;
        case TAD_OP_ECKM:
            content = true;
            if (m.len >= 1 && m.data[0] == 0xFFu)
            {
                f.echo_off = true;
            }
            break;
        default:
            content = true;
            break;
        }
    }
    f.owes = content && !rfi;
    return f;
}

/* Build and commit one frame of this kind. */
static int send_kind(TadResponder *r, TadResponderSend kind, uint32_t now_ms)
{
    XmsgNode *n = r->adm.base.node;
    XmsgSubhdr sub;
    uint8_t *body;
    size_t cap;
    size_t clen = 0;
    size_t len;
    uint16_t flags1;
    ChainFacts facts = {false, false};
    const TadResponderChain *c = &r->chains[r->c_head];
    int rc;

    rc = xmsg_node_send_begin(n, &body, &cap);
    if (rc != XMSG_OK)
    {
        return rc;
    }
    memset(&sub, 0, sizeof sub);
    sub.dst_system = r->caller.system;
    sub.dst_port = r->caller.port;
    sub.src_port = r->term.base.local_port;
    sub.role = TAD_RESP_ROLE_TERMINAL;
    sub.xmcsm = TAD_RESP_XMCSM_TERMINAL;
    sub.frame_flags = XMSG_FF_DATA_A;

    switch (kind)
    {
    case RS_ACCEPT:
        /* MEASURED, record 294 and 16 more: 00 41 00 08 01 02 00 00 02 02 00 0A. */
        {
            static const uint8_t accept[] = {0x00, 0x41, 0x00, 0x08, 0x01, 0x02,
                                             0x00, 0x00, 0x02, 0x02, 0x00, 0x0A};
            memcpy(body, accept, sizeof accept);
            len = sizeof accept;
        }
        sub.frame_flags = XMSG_FF_SETUP;
        sub.role = TAD_RESP_ROLE_ADM;
        sub.src_port = r->adm.base.local_port;
        sub.xmcsm = TAD_RESP_XMCSM_LETTER;
        break;
    case RS_PORT_ASSIGN:
        clen = build_port_assign(r, body + TAD_BODY_HEAD_SIZE, cap - TAD_BODY_HEAD_SIZE);
        len = TAD_BODY_HEAD_SIZE + clen;
        sub.frame_flags = XMSG_FF_SETUP;
        sub.role = TAD_RESP_ROLE_ADM;
        sub.src_port = r->adm.base.local_port;
        sub.xmcsm = TAD_RESP_XMCSM_LETTER;
        break;
    case RS_DUMM:
        clen = build_bare((uint8_t)TAD_OP_DUMM, body + TAD_BODY_HEAD_SIZE, cap - 4u);
        len = TAD_BODY_HEAD_SIZE + clen;
        sub.frame_flags = XMSG_FF_DATA_B; /* MEASURED 0x92 in all 7 ladders */
        break;
    case RS_ESRS:
    case RS_EDRS:
        /* ESRS MEASURED (record 300: 0x86, role 00, 0x0008); EDRS shaped the same, unverified. */
        clen = build_bare((kind == RS_ESRS) ? (uint8_t)TAD_OP_ESRS : (uint8_t)TAD_OP_EDRS,
                          body + TAD_BODY_HEAD_SIZE, cap - 4u);
        len = TAD_BODY_HEAD_SIZE + clen;
        sub.frame_flags = XMSG_FF_SETUP;
        sub.xmcsm = TAD_RESP_XMCSM_BARE;
        break;
    case RS_RESE_1:
    case RS_RESE_2:
        clen = build_bare((uint8_t)TAD_OP_RESE, body + TAD_BODY_HEAD_SIZE, cap - 4u);
        len = TAD_BODY_HEAD_SIZE + clen;
        /* MEASURED in all 7 ladders: the first 0x96, the second 0x92. */
        sub.frame_flags = (kind == RS_RESE_1) ? XMSG_FF_DATA_A : XMSG_FF_DATA_B;
        break;
    case RS_CHAIN:
        memcpy(body + TAD_BODY_HEAD_SIZE, c->bytes, c->len);
        clen = c->len;
        len = TAD_BODY_HEAD_SIZE + clen;
        facts = chain_facts(c->bytes, c->len);
        sub.frame_flags = facts.echo_off ? XMSG_FF_DATA_B : XMSG_FF_DATA_A;
        break;
    case RS_NOTICE:
        /* MEASURED, record 341 and 1327: 0x82, role 0x54, 0x0006, FD 00, from *TADADM. */
        clen = build_bare((uint8_t)TAD_OP_RAW_FD, body + TAD_BODY_HEAD_SIZE, cap - 4u);
        len = TAD_BODY_HEAD_SIZE + clen;
        sub.frame_flags = XMSG_FF_CONTROL_BARE;
        sub.role = TAD_RESP_ROLE_NOTICE;
        sub.src_port = r->adm.base.local_port;
        sub.xmcsm = TAD_RESP_XMCSM_NOTICE;
        break;
    case RS_DCON:
        /* NOT seen from a real host; the C# TadServer's DCON, live-verified on D100 by the C#. */
        clen = build_bare((uint8_t)TAD_OP_DCON, body + TAD_BODY_HEAD_SIZE, cap - 4u);
        len = TAD_BODY_HEAD_SIZE + clen;
        sub.frame_flags = XMSG_FF_CONTROL_BARE;
        sub.xmcsm = TAD_RESP_XMCSM_BARE;
        break;
    default:
        xmsg_node_send_cancel(n);
        return XMSG_EINVAL;
    }
    if (kind != RS_ACCEPT)
    {
        if (clen == 0)
        {
            xmsg_node_send_cancel(n);
            return XMSG_EINVAL;
        }
        tad_body_head_write(body, 0, (uint16_t)clen);
    }
    rc = xmsg_node_send_commit(n, r->caller.peer_node, &sub, len, now_ms, &flags1);
    if (rc != XMSG_OK)
    {
        xmsg_node_send_cancel(n);
        return rc;
    }
    r->inflight = (uint8_t)kind;
    r->inflight_flags1 = flags1;
    r->inflight_since_ms = now_ms;
    r->stats.frames_out++;
    if (kind == RS_CHAIN)
    {
        TadReader rd;
        TadMessage m;
        tad_reader_init(&rd, c->bytes, c->len);
        while (tad_reader_next(&rd, &m))
        {
            if (m.opcode == TAD_OP_BDAT)
            {
                r->stats.text_bytes_out += m.len;
            }
        }
        if (facts.owes && r->state == TAD_RESP_RUNNING)
        {
            r->answers_owed++;
        }
    }
    return XMSG_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* Session operations (the *TADADM port carries the sending and the timers)                   */
/* ------------------------------------------------------------------------------------------ */

static int resp_poll_tx(XmsgSession *s, uint32_t now_ms)
{
    TadResponder *r = ((TadResponderPort *)s)->owner;
    TadResponderSend kind;
    int rc;

    kind = (r->inflight == RS_NONE) ? next_kind(r) : RS_NONE;
    if (kind == RS_NONE)
    {
        update_wants_tx(r);
        return 0;
    }
    if (kind == RS_CHAIN && r->c_closed == 0 && close_chain(r) != XMSG_OK)
    {
        update_wants_tx(r);
        return 0;
    }
    rc = send_kind(r, kind, now_ms);
    if (rc == XMSG_EAGAIN || rc == XMSG_ESTATE)
    {
        return 0; /* window full or link settling: next poll */
    }
    if (rc != XMSG_OK)
    {
        fail(r, rc, now_ms);
        return 0;
    }
    if (kind != RS_CHAIN)
    {
        r->q_head = (uint8_t)((r->q_head + 1u) % TAD_RESP_QUEUE);
        r->q_count--;
    }
    update_wants_tx(r);
    return 1;
}

static void resp_on_delivered(XmsgSession *s, uint16_t flags1)
{
    TadResponder *r = ((TadResponderPort *)s)->owner;
    TadResponderSend done = (TadResponderSend)r->inflight;

    if (done == RS_NONE || flags1 != r->inflight_flags1)
    {
        return;
    }
    r->inflight = RS_NONE;
    r->xense_tries = 0;
    if (done == RS_CHAIN)
    {
        r->chains[r->c_head].len = 0;
        r->chains[r->c_head].bdat_at = -1;
        r->c_head = (uint8_t)((r->c_head + 1u) % TAD_RESP_CHAINS);
        r->c_closed--;
    }
    if (done == RS_DCON)
    {
        set_state(r, TAD_RESP_CLOSED, TAD_RESP_END_LOCAL, r->inflight_since_ms);
        return;
    }
    update_wants_tx(r);
}

static void requeue_inflight(TadResponder *r)
{
    if (r->inflight == RS_CHAIN)
    {
        /* The chain is still at the head of the ring; it is counted again when sent again. */
        if (r->answers_owed > 0 && r->state == TAD_RESP_RUNNING)
        {
            r->answers_owed--;
        }
    }
    else
    {
        r->q_head = (uint8_t)((r->q_head + TAD_RESP_QUEUE - 1u) % TAD_RESP_QUEUE);
        r->queue[r->q_head] = r->inflight;
        r->q_count++;
    }
    r->inflight = RS_NONE;
    update_wants_tx(r);
}

static void resp_on_peer_restart(XmsgSession *s)
{
    TadResponder *r = ((TadResponderPort *)s)->owner;

    if (r->inflight == RS_NONE)
    {
        return;
    }
    XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_INFO, "asker restarted its count; frame 0x%04X sent again",
             r->inflight_flags1);
    r->stats.restart_resends++;
    requeue_inflight(r);
}

static void resp_on_error(XmsgSession *s, int16_t code, uint16_t flags1)
{
    TadResponder *r = ((TadResponderPort *)s)->owner;

    if (r->inflight == RS_NONE || flags1 != r->inflight_flags1)
    {
        return;
    }
    if (code == XMSG_XE_XENSE && r->xense_tries < TAD_RESP_XENSE_TRIES)
    {
        r->xense_tries++;
        r->stats.xense_resends++;
        requeue_inflight(r);
        return;
    }
    XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_ERROR, "responder frame 0x%04X refused by the asker: %d",
             flags1, (int)code);
    fail(r, code, r->inflight_since_ms);
}

static void resp_tick(XmsgSession *s, uint32_t now_ms)
{
    TadResponder *r = ((TadResponderPort *)s)->owner;

    if ((r->state == TAD_RESP_ACCEPTING || r->state == TAD_RESP_BRINGUP) &&
        (uint32_t)(now_ms - r->state_since_ms) >= r->cfg.setup_timeout_ms)
    {
        XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_ERROR, "bring-up not finished in %u ms (state %s)",
                 (unsigned)r->cfg.setup_timeout_ms, tad_responder_state_name(r->state));
        fail(r, XMSG_ETIMEDOUT, now_ms);
        return;
    }
    if (r->inflight != RS_NONE &&
        (uint32_t)(now_ms - r->inflight_since_ms) >= r->cfg.ack_timeout_ms)
    {
        XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_ERROR, "frame 0x%04X not acknowledged in %u ms",
                 r->inflight_flags1, (unsigned)r->cfg.ack_timeout_ms);
        fail(r, XMSG_ETIMEDOUT, now_ms);
        return;
    }
    update_wants_tx(r);
}

/* The terminal setup: remember what the asker said about its terminal. */
static void note_terminal(TadResponder *r, const uint8_t *chain, size_t clen)
{
    TadReader rd;
    TadMessage m;

    tad_reader_init(&rd, chain, clen);
    while (tad_reader_next(&rd, &m))
    {
        if (m.opcode == TAD_OP_TMOD && m.len >= 1)
        {
            r->terminal.tmod = m.data[0];
        }
        else if (m.opcode == TAD_OP_TTYP && m.len >= 2)
        {
            r->terminal.ttyp[0] = m.data[0];
            r->terminal.ttyp[1] = m.data[1];
        }
        else if (m.opcode == TAD_OP_DESC && m.len >= 1)
        {
            r->terminal.desc = m.data[0];
        }
        else if (m.opcode == TAD_OP_OPSV && m.len >= 3)
        {
            memcpy(r->terminal.opsv, m.data, 3);
        }
    }
    r->terminal.seen = true;
}

static void end_by_remote(TadResponder *r, uint32_t now_ms)
{
    XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_INFO, "the asker hung up (DCON)");
    set_state(r, TAD_RESP_CLOSED, TAD_RESP_END_REMOTE, now_ms);
}

/* A frame to the session port during the bring-up. */
static void bringup_frame(TadResponder *r, const uint8_t *chain, size_t clen, uint32_t now_ms)
{
    if (tad_chain_has(chain, clen, TAD_OP_TMOD))
    {
        note_terminal(r, chain, clen); /* no answer: the real host sends none */
    }
    if (tad_chain_has(chain, clen, TAD_OP_ESCA))
    {
        enqueue(r, RS_ESRS, now_ms);
        enqueue(r, RS_RESE_1, now_ms);
        r->reco_count = 0;
        return;
    }
    if (tad_chain_has(chain, clen, TAD_OP_RECO))
    {
        r->reco_count++;
        if (r->reco_count == 1)
        {
            enqueue(r, RS_RESE_2, now_ms);
            return;
        }
        set_state(r, TAD_RESP_RUNNING, 0, now_ms);
        update_wants_tx(r);
    }
}

/* A frame to the session port while running. */
static void running_frame(TadResponder *r, const uint8_t *chain, size_t clen, uint32_t now_ms)
{
    TadReader rd;
    TadMessage m;

    tad_reader_init(&rd, chain, clen);
    while (tad_reader_next(&rd, &m))
    {
        switch (m.opcode)
        {
        case TAD_OP_BDAT:
            if (m.len > 0)
            {
                uint8_t keys[TAD_MAX_DATA];
                for (uint8_t i = 0; i < m.len; i++)
                {
                    keys[i] = r->cfg.strip_parity ? (uint8_t)(m.data[i] & 0x7Fu) : m.data[i];
                }
                r->input_granted = false;
                r->stats.text_bytes_in += m.len;
                if (r->cb.text_in != NULL)
                {
                    r->cb.text_in(r->cb.ctx, keys, m.len);
                }
            }
            break;
        case TAD_OP_DUMM:
        case TAD_OP_CERS:
        case TAD_OP_RECO:
            r->stats.answers_in++;
            if (r->answers_owed > 0)
            {
                r->answers_owed--;
            }
            break;
        case TAD_OP_ESCA:
            if (r->escape_enabled)
            {
                r->stats.escapes++;
                enqueue(r, RS_ESRS, now_ms);
                if (r->cb.escape != NULL)
                {
                    r->cb.escape(r->cb.ctx);
                }
            }
            else
            {
                r->stats.escapes_off++;
                enqueue(r, RS_EDRS, now_ms);
            }
            break;
        default:
            XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_DEBUG, "asker TAD opcode 0x%02X, %u bytes: no answer",
                     m.opcode, m.count);
            break;
        }
    }
    update_wants_tx(r);
}

static void resp_on_datagram(XmsgSession *s, const XmsgDatagram *dg, uint32_t now_ms)
{
    TadResponderPort *p = (TadResponderPort *)s;
    TadResponder *r = p->owner;
    uint16_t lead;
    const uint8_t *chain;
    size_t clen;

    if (dg->repeat)
    {
        r->stats.repeats_in++; /* the node ACKed it again; it was answered the first time */
        return;
    }
    r->stats.frames_in++;
    if (tad_body_split(dg->body, dg->body_len, &lead, &chain, &clen) != XMSG_OK)
    {
        XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_WARN, "asker frame of %u bytes is too short",
                 (unsigned)dg->body_len);
        return;
    }
    if (tad_chain_has(chain, clen, TAD_OP_DCON))
    {
        end_by_remote(r, now_ms);
        return;
    }
    if (p == &r->adm)
    {
        /* The session setup (06 1B 1C FF). Anything else to *TADADM is not expected. */
        if (r->state == TAD_RESP_ACCEPTING && tad_chain_has(chain, clen, TAD_OP_RAW_06))
        {
            set_state(r, TAD_RESP_BRINGUP, 0, now_ms);
            r->reco_count = 0;
            enqueue(r, RS_PORT_ASSIGN, now_ms);
            enqueue(r, RS_DUMM, now_ms);
        }
        else
        {
            XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_DEBUG, "frame to *TADADM in state %s ignored",
                     tad_responder_state_name(r->state));
        }
        return;
    }
    switch (r->state)
    {
    case TAD_RESP_BRINGUP:
        bringup_frame(r, chain, clen, now_ms);
        break;
    case TAD_RESP_RUNNING:
    case TAD_RESP_CLOSING:
        running_frame(r, chain, clen, now_ms);
        break;
    default:
        break;
    }
}

static const XmsgSessionOps s_adm_ops = {
    .on_datagram = resp_on_datagram,
    .poll_tx = resp_poll_tx,
    .tick = resp_tick,
    .on_delivered = resp_on_delivered,
    .on_error = resp_on_error,
    .on_peer_restart = resp_on_peer_restart,
};

/* The session port only receives; ACKs, errors and timers are handled once, on *TADADM. */
static const XmsgSessionOps s_term_ops = {
    .on_datagram = resp_on_datagram,
    .poll_tx = NULL,
    .tick = NULL,
    .on_delivered = NULL,
    .on_error = NULL,
    .on_peer_restart = NULL,
};

/* ------------------------------------------------------------------------------------------ */
/* Letters                                                                                    */
/* ------------------------------------------------------------------------------------------ */

static void copy_param(char *out, size_t size, const XroutParam *p)
{
    size_t n = p->len;

    if (n >= size)
    {
        n = size - 1u;
    }
    memcpy(out, p->data, n);
    out[n] = '\0';
}

/* Read the letter: XSLET, string 1 = "*TADADM"; string 2 (the target) when present. */
static bool read_letter(const XmsgDatagram *dg, TadResponderCaller *who, int32_t *param4)
{
    XroutMessage m;
    const XroutParam *service = NULL;
    const XroutParam *target = NULL;
    size_t name_len = sizeof TAD_RESP_SERVICE_NAME - 1u;

    if (dg == NULL || !dg->has_subhdr || dg->sub.dst_port != 0)
    {
        return false;
    }
    if (xrout_parse(dg->body, dg->body_len, &m) != XMSG_OK || m.service != XROUT_SERVICE_XSLET)
    {
        return false;
    }
    if (param4 != NULL)
    {
        *param4 = xrout_param_int(&m, 4);
    }
    for (uint8_t i = 0; i < m.param_count; i++)
    {
        if (m.params[i].is_string && m.params[i].number == 1)
        {
            service = &m.params[i];
        }
        else if (m.params[i].is_string && m.params[i].number == 2)
        {
            target = &m.params[i];
        }
    }
    if (service == NULL || service->len != name_len ||
        memcmp(service->data, TAD_RESP_SERVICE_NAME, name_len) != 0)
    {
        return false;
    }
    if (who != NULL)
    {
        memset(who, 0, sizeof *who);
        who->peer_node = dg->hdr.src_node;
        who->system = dg->sub.src_system;
        who->port = dg->sub.src_port;
        copy_param(who->service, sizeof who->service, service);
        if (target != NULL)
        {
            copy_param(who->target, sizeof who->target, target);
        }
    }
    return true;
}

bool tad_responder_is_connect(const XmsgDatagram *dg)
{
    return read_letter(dg, NULL, NULL);
}

bool tad_responder_is_status_query(const XmsgDatagram *dg)
{
    int32_t p4 = -1;

    return read_letter(dg, NULL, &p4) && p4 >= 0;
}

int tad_responder_status_reply(XmsgNode *node, const XmsgDatagram *dg, uint16_t in_use,
                               uint16_t free_count, uint32_t now_ms)
{
    XmsgSubhdr sub;
    uint8_t *body;
    size_t cap;
    int rc;

    if (node == NULL || dg == NULL || !dg->has_subhdr)
    {
        return XMSG_EINVAL;
    }
    rc = xmsg_node_send_begin(node, &body, &cap);
    if (rc != XMSG_OK)
    {
        return rc;
    }
    /* The accept's shape (records 105, 381, 455: 00 41 00 08 01 02 <in use> 02 02 <free>). */
    xrout_head_write(body, 0, XROUT_SERVICE_XSLET, 8);
    body[4] = 0x01;
    body[5] = 0x02;
    xmsg_be16_write(body + 6, in_use);
    body[8] = 0x02;
    body[9] = 0x02;
    xmsg_be16_write(body + 10, free_count);
    memset(&sub, 0, sizeof sub);
    sub.frame_flags = XMSG_FF_SETUP;
    sub.role = TAD_RESP_ROLE_ADM;
    sub.dst_system = dg->sub.src_system;
    sub.dst_port = dg->sub.src_port;
    sub.src_port = TAD_RESP_STATUS_PORT;
    sub.xmcsm = TAD_RESP_XMCSM_LETTER;
    rc = xmsg_node_send_commit(node, dg->hdr.src_node, &sub, 12, now_ms, NULL);
    if (rc != XMSG_OK)
    {
        xmsg_node_send_cancel(node);
    }
    return rc;
}

int tad_responder_refuse(XmsgNode *node, const XmsgDatagram *dg, uint8_t code, uint32_t now_ms)
{
    XmsgSubhdr sub;
    uint8_t *body;
    size_t cap;
    int rc;

    if (node == NULL || dg == NULL || !dg->has_subhdr || code == 0)
    {
        return XMSG_EINVAL;
    }
    rc = xmsg_node_send_begin(node, &body, &cap);
    if (rc != XMSG_OK)
    {
        return rc;
    }
    /* UNVERIFIED: no captured refusal. The accept's head with the service byte replaced by the
     * XROUT error code (xrout_letter.h), no parameters, from XROUT's port 0, as the accept. */
    xrout_head_write(body, 0, code, 0);
    memset(&sub, 0, sizeof sub);
    sub.frame_flags = XMSG_FF_SETUP;
    sub.role = TAD_RESP_ROLE_ADM;
    sub.dst_system = dg->sub.src_system;
    sub.dst_port = dg->sub.src_port;
    sub.src_port = 0;
    sub.xmcsm = TAD_RESP_XMCSM_LETTER;
    rc = xmsg_node_send_commit(node, dg->hdr.src_node, &sub, XROUT_HEAD_SIZE, now_ms, NULL);
    if (rc != XMSG_OK)
    {
        xmsg_node_send_cancel(node);
    }
    return rc;
}

/* ------------------------------------------------------------------------------------------ */
/* Public API                                                                                 */
/* ------------------------------------------------------------------------------------------ */

void tad_responder_config_default(TadResponderConfig *cfg)
{
    if (cfg == NULL)
    {
        return;
    }
    memset(cfg, 0, sizeof *cfg);
    cfg->logical_unit = 0x0300u;
    cfg->strip_parity = true;
    cfg->ack_timeout_ms = 10000;
    cfg->setup_timeout_ms = 30000;
}

int tad_responder_init(TadResponder *r, const TadResponderConfig *cfg,
                       const TadResponderCallbacks *cb)
{
    if (r == NULL || cfg == NULL)
    {
        return XMSG_EINVAL;
    }
    memset(r, 0, sizeof *r);
    r->cfg = *cfg;
    if (cb != NULL)
    {
        r->cb = *cb;
    }
    r->adm.base.ops = &s_adm_ops;
    r->adm.owner = r;
    r->term.base.ops = &s_term_ops;
    r->term.owner = r;
    r->chains[0].bdat_at = -1;
    r->state = TAD_RESP_IDLE;
    return XMSG_OK;
}

static bool in_use(const TadResponder *r)
{
    return r->state != TAD_RESP_IDLE && r->state != TAD_RESP_CLOSED &&
           r->state != TAD_RESP_FAILED;
}

int tad_responder_offer(TadResponder *r, XmsgNode *node, const XmsgDatagram *dg,
                        uint32_t now_ms)
{
    TadResponderCaller who;
    int32_t p4 = -1;
    int verdict = 0;
    int rc;

    if (r == NULL || node == NULL || dg == NULL)
    {
        return XMSG_EINVAL;
    }
    if (!read_letter(dg, &who, &p4))
    {
        return 0;
    }
    if (p4 >= 0)
    {
        /* A status question (LI-SYS), not a call: answered, no session (see the header). The
         * counts come from the application; without its callback this responder only knows
         * itself. */
        uint16_t busy = in_use(r) ? 1u : 0u;
        uint16_t total = TAD_RESP_STATUS_POOL;
        if (r->cb.status != NULL)
        {
            r->cb.status(r->cb.ctx, &busy, &total);
        }
        if (busy > total)
        {
            busy = total; /* never a wrapped free count on the wire */
        }
        r->stats.status_queries++;
        if (!dg->repeat)
        {
            (void)tad_responder_status_reply(node, dg, busy, (uint16_t)(total - busy), now_ms);
        }
        return 1;
    }
    if (in_use(r))
    {
        if (who.peer_node == r->caller.peer_node && who.system == r->caller.system &&
            who.port == r->caller.port)
        {
            r->stats.repeats_in++; /* the same caller's letter again: answered already */
            return 1;
        }
        return XMSG_EAGAIN;
    }
    if (dg->repeat)
    {
        return 1; /* answered (or refused) when it first came */
    }
    if (r->cb.incoming != NULL)
    {
        verdict = r->cb.incoming(r->cb.ctx, &who);
    }
    r->stats.letters++;
    if (verdict != 0)
    {
        r->stats.refused++;
        XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_INFO, "call from system %u refused: XROUT %d",
                 who.system, verdict);
        (void)tad_responder_refuse(node, dg, (uint8_t)verdict, now_ms);
        return 1;
    }

    memset(&r->adm.base, 0, sizeof r->adm.base);
    memset(&r->term.base, 0, sizeof r->term.base);
    r->adm.base.ops = &s_adm_ops;
    r->term.base.ops = &s_term_ops;
    r->adm.base.peer_node = who.peer_node;
    r->adm.base.peer_system = who.system;
    r->adm.base.peer_port = who.port;
    r->term.base.peer_node = who.peer_node;
    r->term.base.peer_system = who.system;
    r->term.base.peer_port = who.port;
    rc = xmsg_node_attach(node, &r->adm.base);
    if (rc == XMSG_OK)
    {
        rc = xmsg_node_attach(node, &r->term.base);
        if (rc != XMSG_OK)
        {
            (void)xmsg_node_detach(node, &r->adm.base);
        }
    }
    if (rc != XMSG_OK)
    {
        XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_WARN, "no session place for a call: refused (XRNSP)");
        (void)tad_responder_refuse(node, dg, 4u, now_ms);
        return XMSG_ENOSPC;
    }

    r->caller = who;
    memset(&r->terminal, 0, sizeof r->terminal);
    r->q_head = 0;
    r->q_count = 0;
    r->c_head = 0;
    r->c_closed = 0;
    r->chains[0].len = 0;
    r->chains[0].bdat_at = -1;
    r->inflight = RS_NONE;
    r->xense_tries = 0;
    r->reco_count = 0;
    r->escape_enabled = true;
    r->input_granted = false;
    r->answers_owed = 0;
    XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_INFO,
             "call from node %u system %u port 0x%04X for \"%s\": *TADADM 0x%04X, session 0x%04X",
             who.peer_node, who.system, who.port, who.target, r->adm.base.local_port,
             r->term.base.local_port);
    r->state = TAD_RESP_IDLE;
    set_state(r, TAD_RESP_ACCEPTING, 0, now_ms);
    enqueue(r, RS_ACCEPT, now_ms);
    return 1;
}

static bool may_add(const TadResponder *r)
{
    return r->state == TAD_RESP_ACCEPTING || r->state == TAD_RESP_BRINGUP ||
           r->state == TAD_RESP_RUNNING;
}

int tad_responder_write(TadResponder *r, const uint8_t *bytes, size_t len)
{
    size_t taken = 0;

    if (r == NULL || bytes == NULL || len == 0)
    {
        return XMSG_EINVAL;
    }
    if (!may_add(r))
    {
        return XMSG_ESTATE;
    }
    while (taken < len)
    {
        TadResponderChain *c = open_chain(r);
        size_t room;
        size_t n;

        if (c == NULL)
        {
            break;
        }
        if (c->bdat_at < 0)
        {
            /* A new BDAT: on an even offset, as every host BDAT is. */
            size_t need = (size_t)(c->len & 1u) + 2u + 1u;
            c = chain_with_room(r, need);
            if (c == NULL)
            {
                break;
            }
            if ((c->len & 1u) != 0)
            {
                c->bytes[c->len++] = 0x00;
            }
            c->bdat_at = (int16_t)c->len;
            c->bytes[c->len++] = (uint8_t)TAD_OP_BDAT;
            c->bytes[c->len++] = 0;
        }
        room = TAD_MAX_DATA - c->bytes[c->bdat_at + 1];
        if (room > TAD_RESP_CHAIN_MAX - c->len)
        {
            room = TAD_RESP_CHAIN_MAX - c->len;
        }
        n = len - taken;
        if (n > room)
        {
            n = room;
        }
        memcpy(c->bytes + c->len, bytes + taken, n);
        c->len = (uint16_t)(c->len + n);
        c->bytes[c->bdat_at + 1] = (uint8_t)(c->bytes[c->bdat_at + 1] + n);
        taken += n;
        if (c->bytes[c->bdat_at + 1] == TAD_MAX_DATA || c->len >= TAD_RESP_CHAIN_MAX)
        {
            /* A full BDAT ends its frame (the real host's listing: one 255-byte BDAT each). */
            if (close_chain(r) != XMSG_OK)
            {
                break;
            }
        }
    }
    update_wants_tx(r);
    return (taken > 0) ? (int)taken : XMSG_EAGAIN;
}

int tad_responder_put(TadResponder *r, uint8_t opcode, const uint8_t *data, size_t len)
{
    TadResponderChain *c;
    TadWriter w;
    int rc;

    if (r == NULL || len > TAD_MAX_DATA || (len > 0 && data == NULL))
    {
        return XMSG_EINVAL;
    }
    if (!may_add(r))
    {
        return XMSG_ESTATE;
    }
    c = chain_with_room(r, 1u + 2u + len);
    if (c == NULL)
    {
        return XMSG_EAGAIN;
    }
    tad_writer_init(&w, c->bytes, TAD_RESP_CHAIN_MAX);
    w.len = c->len;
    rc = tad_writer_add_host(&w, opcode, data, len);
    if (rc != XMSG_OK)
    {
        return rc;
    }
    c->len = (uint16_t)w.len;
    c->bdat_at = -1; /* text after this is a new BDAT */
    if (opcode == TAD_OP_CESC && len >= 1)
    {
        r->escape_enabled = data[0] != 0;
    }
    if (opcode == TAD_OP_RFI)
    {
        r->input_granted = true;
    }
    update_wants_tx(r);
    return XMSG_OK;
}

int tad_responder_echo(TadResponder *r, uint8_t eckm)
{
    return tad_responder_put(r, (uint8_t)TAD_OP_ECKM, &eckm, 1);
}

int tad_responder_login_state(TadResponder *r, uint16_t sycn)
{
    uint8_t d[2];

    xmsg_be16_write(d, sycn);
    return tad_responder_put(r, (uint8_t)TAD_OP_SYCN, d, sizeof d);
}

int tad_responder_set_escape(TadResponder *r, bool enabled)
{
    uint8_t d = enabled ? 0x01u : 0x00u;

    return tad_responder_put(r, (uint8_t)TAD_OP_CESC, &d, 1);
}

int tad_responder_grant_input(TadResponder *r)
{
    return tad_responder_put(r, (uint8_t)TAD_OP_RFI, NULL, 0);
}

int tad_responder_send(TadResponder *r)
{
    int rc;

    if (r == NULL)
    {
        return XMSG_EINVAL;
    }
    if (!may_add(r))
    {
        return XMSG_ESTATE;
    }
    rc = close_chain(r);
    update_wants_tx(r);
    return rc;
}

bool tad_responder_input_granted(const TadResponder *r)
{
    return r != NULL && r->state == TAD_RESP_RUNNING && r->input_granted;
}

int tad_responder_hangup(TadResponder *r, uint32_t now_ms)
{
    if (r == NULL)
    {
        return XMSG_EINVAL;
    }
    switch (r->state)
    {
    case TAD_RESP_ACCEPTING:
    case TAD_RESP_BRINGUP:
        /* Nothing the application wrote has gone yet; drop it. The 0xFD and DCON follow what
         * the bring-up still has waiting (the port assignment names the session port). */
        drop_chains(r);
        break;
    case TAD_RESP_RUNNING:
        (void)close_chain(r);
        r->answers_owed = 0; /* the last text goes without waiting for the asker's answers */
        break;
    default:
        return XMSG_ESTATE;
    }
    r->input_granted = false;
    set_state(r, TAD_RESP_CLOSING, 0, now_ms);
    enqueue(r, RS_NOTICE, now_ms);
    enqueue(r, RS_DCON, now_ms);
    return XMSG_OK;
}

void tad_responder_abort(TadResponder *r)
{
    if (r == NULL || !in_use(r))
    {
        return;
    }
    XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_WARN, "responder dropped locally in state %s; nothing sent",
             tad_responder_state_name(r->state));
    set_state(r, TAD_RESP_CLOSED, TAD_RESP_END_ABORT, r->state_since_ms);
}

TadResponderState tad_responder_state(const TadResponder *r)
{
    return (r == NULL) ? TAD_RESP_IDLE : r->state;
}

const char *tad_responder_state_name(TadResponderState s)
{
    switch (s)
    {
    case TAD_RESP_IDLE:
        return "idle";
    case TAD_RESP_ACCEPTING:
        return "accepting";
    case TAD_RESP_BRINGUP:
        return "bring-up";
    case TAD_RESP_RUNNING:
        return "running";
    case TAD_RESP_CLOSING:
        return "closing";
    case TAD_RESP_CLOSED:
        return "closed";
    case TAD_RESP_FAILED:
        return "failed";
    default:
        return "?";
    }
}
