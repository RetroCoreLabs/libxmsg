/*
 * tad_asker.c - a terminal session to a SINTRAN host.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/tad/tad_asker.h"

#include <string.h>

#include "libxmsg/log/xmsg_log.h"
#include "libxmsg/xmsg_endian.h"
#include "libxmsg/xrout/xrout_letter.h"

/* ------------------------------------------------------------------------------------------ */
/* State and queue                                                                            */
/* ------------------------------------------------------------------------------------------ */

static void update_wants_tx(TadAsker *a)
{
    a->base.wants_tx = (a->inflight == TAD_SEND_NONE) && (a->q_count > 0);
}

static void set_state(TadAsker *a, TadAskerState st, int error, uint32_t now_ms)
{
    if (a->state == st)
    {
        return;
    }
    XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_INFO, "asker %s -> %s (error %d)",
             tad_asker_state_name(a->state), tad_asker_state_name(st), error);
    a->state = st;
    a->error = error;
    a->state_since_ms = now_ms;
    if (st == TAD_ASKER_CLOSED || st == TAD_ASKER_FAILED)
    {
        a->q_count = 0;
        a->inflight = TAD_SEND_NONE;
        a->rfi_open = false;
        a->base.wants_tx = false;
        if (a->base.attached && a->base.node != NULL)
        {
            (void)xmsg_node_detach(a->base.node, &a->base); /* safe inside node callbacks */
        }
    }
    if (a->cb.state != NULL)
    {
        a->cb.state(a->cb.ctx, st, error);
    }
}

static void fail(TadAsker *a, int error, uint32_t now_ms)
{
    set_state(a, TAD_ASKER_FAILED, error, now_ms);
}

static void enqueue(TadAsker *a, TadAskerSend kind, uint32_t now_ms)
{
    if (a->q_count >= TAD_ASKER_QUEUE)
    {
        XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_ERROR,
                 "asker reply queue full (%u): the host sent faster than we answer",
                 (unsigned)TAD_ASKER_QUEUE);
        fail(a, XMSG_ENOSPC, now_ms);
        return;
    }
    a->queue[(a->q_head + a->q_count) % TAD_ASKER_QUEUE] = (uint8_t)kind;
    a->q_count++;
    update_wants_tx(a);
}

static void set_rfi(TadAsker *a, bool open)
{
    if (a->rfi_open == open)
    {
        return;
    }
    a->rfi_open = open;
    if (a->cb.input_gate != NULL)
    {
        a->cb.input_gate(a->cb.ctx, open);
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Building frames                                                                            */
/* ------------------------------------------------------------------------------------------ */

/* Put a packed chain at body + 4 and the head in front of it. Returns the body length or 0. */
static size_t build_chain(TadAsker *a, TadAskerSend kind, uint8_t *body, size_t cap)
{
    TadWriter w;
    static const uint8_t setup_1c = 0x00;

    if (cap < TAD_BODY_HEAD_SIZE)
    {
        return 0;
    }
    tad_writer_init(&w, body + TAD_BODY_HEAD_SIZE, cap - TAD_BODY_HEAD_SIZE);
    switch (kind)
    {
    case TAD_SEND_SETUP:
        /* MEASURED: 06 00 1B 00 1C 01 00 FF 00 in every captured session setup. */
        (void)tad_writer_add(&w, (uint8_t)TAD_OP_RAW_06, NULL, 0);
        (void)tad_writer_add(&w, (uint8_t)TAD_OP_RAW_1B, NULL, 0);
        (void)tad_writer_add(&w, (uint8_t)TAD_OP_RAW_1C, &setup_1c, 1);
        (void)tad_writer_add(&w, (uint8_t)TAD_OP_RAW_FF, NULL, 0);
        break;
    case TAD_SEND_TERMINAL_SETUP:
        (void)tad_writer_add(&w, (uint8_t)TAD_OP_TMOD, &a->cfg.tmod, 1);
        (void)tad_writer_add(&w, (uint8_t)TAD_OP_TTYP, a->cfg.ttyp, 2);
        (void)tad_writer_add(&w, (uint8_t)TAD_OP_DESC, &a->cfg.desc, 1);
        (void)tad_writer_add(&w, (uint8_t)TAD_OP_OPSV, a->cfg.opsv, 3);
        break;
    case TAD_SEND_ESCA:
        (void)tad_writer_add(&w, (uint8_t)TAD_OP_ESCA, NULL, 0);
        break;
    case TAD_SEND_RECO:
        (void)tad_writer_add(&w, (uint8_t)TAD_OP_RECO, NULL, 0);
        break;
    case TAD_SEND_CERS:
        (void)tad_writer_add(&w, (uint8_t)TAD_OP_CERS, NULL, 0);
        break;
    case TAD_SEND_DUMM:
        (void)tad_writer_add(&w, (uint8_t)TAD_OP_DUMM, NULL, 0);
        break;
    case TAD_SEND_TEXT:
        (void)tad_writer_add(&w, (uint8_t)TAD_OP_BDAT, a->line, a->line_len);
        break;
    case TAD_SEND_DCON:
        (void)tad_writer_add(&w, (uint8_t)TAD_OP_DCON, NULL, 0);
        break;
    default:
        return 0;
    }
    if (tad_writer_length(&w) == 0)
    {
        return 0;
    }
    tad_body_head_write(body, 0, (uint16_t)tad_writer_length(&w));
    return TAD_BODY_HEAD_SIZE + tad_writer_length(&w);
}

/* The sub-header for each kind, as captured. */
static void fill_subhdr(const TadAsker *a, TadAskerSend kind, XmsgSubhdr *sub)
{
    memset(sub, 0, sizeof *sub);
    sub->dst_system = a->base.peer_system;
    sub->src_port = a->base.local_port;
    switch (kind)
    {
    case TAD_SEND_LETTER:
        sub->frame_flags = XMSG_FF_SETUP;
        sub->role = TAD_ROLE_LETTER;
        sub->dst_port = 0;
        sub->xmcsm = TAD_XMCSM_LETTER;
        break;
    case TAD_SEND_SETUP:
        sub->frame_flags = XMSG_FF_SETUP;
        sub->role = TAD_ROLE_DATA;
        sub->dst_port = a->tadadm_port;
        sub->xmcsm = TAD_XMCSM_SETUP;
        break;
    case TAD_SEND_TERMINAL_SETUP:
        sub->frame_flags = XMSG_FF_SETUP;
        sub->role = TAD_ROLE_DATA;
        sub->dst_port = a->session_port;
        sub->xmcsm = TAD_XMCSM_TERMINAL;
        break;
    case TAD_SEND_ESCA:
    case TAD_SEND_DCON:
        sub->frame_flags = XMSG_FF_CONTROL_BARE;
        sub->role = TAD_ROLE_CONTROL;
        sub->dst_port = a->session_port;
        sub->xmcsm = TAD_XMCSM_BARE;
        break;
    case TAD_SEND_TEXT:
        sub->frame_flags = XMSG_FF_DATA_A;
        sub->role = TAD_ROLE_DATA;
        sub->dst_port = a->session_port;
        sub->xmcsm = TAD_XMCSM_TERMINAL;
        break;
    default: /* RECO, CERS, DUMM */
        sub->frame_flags = XMSG_FF_DATA_A;
        sub->role = TAD_ROLE_CONTROL;
        sub->dst_port = a->session_port;
        sub->xmcsm = TAD_XMCSM_TERMINAL;
        break;
    }
}

/* Build and commit one frame of this kind. Returns XMSG_OK, XMSG_EAGAIN or another error. */
static int send_kind(TadAsker *a, TadAskerSend kind, uint32_t now_ms)
{
    XmsgNode *n = a->base.node;
    XmsgSubhdr sub;
    uint8_t *body;
    size_t cap;
    size_t len;
    uint16_t flags1;
    int rc;

    rc = xmsg_node_send_begin(n, &body, &cap);
    if (rc != XMSG_OK)
    {
        return rc;
    }
    if (kind == TAD_SEND_LETTER)
    {
        len = xrout_letter_build(body, cap, TAD_SERVICE_NAME, a->system_name);
    }
    else
    {
        len = build_chain(a, kind, body, cap);
    }
    if (len == 0)
    {
        xmsg_node_send_cancel(n);
        return XMSG_EINVAL;
    }
    fill_subhdr(a, kind, &sub);
    rc = xmsg_node_send_commit(n, a->base.peer_node, &sub, len, now_ms, &flags1);
    if (rc != XMSG_OK)
    {
        xmsg_node_send_cancel(n);
        return rc;
    }
    a->inflight = kind;
    a->inflight_flags1 = flags1;
    a->inflight_since_ms = now_ms;
    a->stats.frames_out++;
    if (kind == TAD_SEND_TEXT)
    {
        a->stats.text_bytes_out += a->line_len;
    }
    return XMSG_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* Session operations                                                                         */
/* ------------------------------------------------------------------------------------------ */

static int asker_poll_tx(XmsgSession *s, uint32_t now_ms)
{
    TadAsker *a = (TadAsker *)s;
    TadAskerSend kind;
    int rc;

    if (a->inflight != TAD_SEND_NONE || a->q_count == 0)
    {
        update_wants_tx(a);
        return 0;
    }
    kind = (TadAskerSend)a->queue[a->q_head];
    rc = send_kind(a, kind, now_ms);
    if (rc == XMSG_EAGAIN || rc == XMSG_ESTATE)
    {
        return 0; /* the window is full or the link is settling: try on the next poll */
    }
    if (rc != XMSG_OK)
    {
        fail(a, rc, now_ms);
        return 0;
    }
    a->q_head = (uint8_t)((a->q_head + 1u) % TAD_ASKER_QUEUE);
    a->q_count--;
    update_wants_tx(a);
    return 1;
}

static void asker_on_delivered(XmsgSession *s, uint16_t flags1)
{
    TadAsker *a = (TadAsker *)s;
    TadAskerSend done = a->inflight;

    if (done == TAD_SEND_NONE || flags1 != a->inflight_flags1)
    {
        return;
    }
    a->inflight = TAD_SEND_NONE;
    a->xense_tries = 0;
    if (done == TAD_SEND_TEXT)
    {
        a->line_len = 0;
    }
    if (done == TAD_SEND_DCON)
    {
        set_state(a, TAD_ASKER_CLOSED, 0, a->inflight_since_ms);
        return;
    }
    update_wants_tx(a);
}

/* Put the frame in flight back at the head of the queue, to be committed again (and numbered
 * again by the node). */
static void requeue_inflight(TadAsker *a)
{
    a->q_head = (uint8_t)((a->q_head + TAD_ASKER_QUEUE - 1u) % TAD_ASKER_QUEUE);
    a->queue[a->q_head] = (uint8_t)a->inflight;
    a->q_count++;
    a->inflight = TAD_SEND_NONE;
    update_wants_tx(a);
}

static void asker_on_peer_restart(XmsgSession *s)
{
    TadAsker *a = (TadAsker *)s;

    if (a->inflight == TAD_SEND_NONE)
    {
        return;
    }
    /* Measured 2026-11-09 against RetroCore D100: our first letter (Flags1 0000) drew a
     * reachability request instead of an ACK and was never acknowledged. */
    XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_INFO, "host restarted its count; frame 0x%04X sent again",
             a->inflight_flags1);
    a->stats.restart_resends++;
    requeue_inflight(a);
}

static void asker_on_error(XmsgSession *s, int16_t code, uint16_t flags1)
{
    TadAsker *a = (TadAsker *)s;

    if (a->inflight == TAD_SEND_NONE || flags1 != a->inflight_flags1)
    {
        return;
    }
    if (code == XMSG_XE_XENSE && a->xense_tries < TAD_ASKER_XENSE_TRIES)
    {
        /* The node has stepped the count down; send the same frame again at the new number. */
        a->xense_tries++;
        a->stats.xense_resends++;
        requeue_inflight(a);
        return;
    }
    XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_ERROR, "asker frame 0x%04X refused by the host: error %d",
             flags1, (int)code);
    fail(a, code, a->inflight_since_ms);
}

static void asker_tick(XmsgSession *s, uint32_t now_ms)
{
    TadAsker *a = (TadAsker *)s;

    if (a->state == TAD_ASKER_CONNECTING &&
        (uint32_t)(now_ms - a->state_since_ms) >= a->cfg.connect_timeout_ms)
    {
        XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_ERROR, "no answer to the connect letter in %u ms",
                 (unsigned)a->cfg.connect_timeout_ms);
        fail(a, XMSG_ETIMEDOUT, now_ms);
        return;
    }
    if (a->inflight != TAD_SEND_NONE &&
        (uint32_t)(now_ms - a->inflight_since_ms) >= a->cfg.ack_timeout_ms)
    {
        XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_ERROR, "frame 0x%04X not acknowledged in %u ms",
                 a->inflight_flags1, (unsigned)a->cfg.ack_timeout_ms);
        fail(a, XMSG_ETIMEDOUT, now_ms);
    }
}

/* The reply rule (see the header): one reply per host terminal frame. */
static void answer_terminal_frame(TadAsker *a, const uint8_t *chain, size_t clen, uint32_t now_ms)
{
    TadReader r;
    TadMessage m;
    bool has_rese = false;
    bool has_rfi = false;
    bool has_cesc = false;
    bool has_fd = false;
    bool has_content = false;

    tad_reader_init(&r, chain, clen);
    while (tad_reader_next(&r, &m))
    {
        switch (m.opcode)
        {
        case TAD_OP_BDAT:
            has_content = true;
            if (m.len > 0)
            {
                a->stats.text_bytes_in += m.len;
                if (a->cb.text_out != NULL)
                {
                    a->cb.text_out(a->cb.ctx, m.data, m.len);
                }
            }
            break;
        case TAD_OP_ECKM:
            has_content = true;
            if (m.len >= 1 && a->cb.echo_mode != NULL)
            {
                a->cb.echo_mode(a->cb.ctx, m.data[0]);
            }
            break;
        case TAD_OP_RFI:
            has_content = true;
            has_rfi = true;
            break;
        case TAD_OP_CESC:
            has_content = true;
            has_cesc = true;
            break;
        case TAD_OP_RESE:
            has_rese = true;
            break;
        case TAD_OP_DUMM:
        case TAD_OP_ESRS:
            break;
        case TAD_OP_RAW_FD:
            has_fd = true;
            break;
        default:
            has_content = true; /* BMMX, SYCN and the rest: content that needs a DUMM */
            XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_DEBUG, "host TAD opcode 0x%02X, %u bytes", m.opcode,
                     m.count);
            break;
        }
    }

    if (has_fd)
    {
        /* A notice, not the end: see the file comment in tad_asker.h. Nothing is sent back;
         * the node has already acknowledged the frame. */
        a->stats.host_notices++;
        XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_INFO, "host session notice (0xFD) number %u",
                 (unsigned)a->stats.host_notices);
        if (a->cb.host_notice != NULL)
        {
            a->cb.host_notice(a->cb.ctx);
        }
        return;
    }
    if (has_rese)
    {
        enqueue(a, TAD_SEND_RECO, now_ms);
        return;
    }
    if (!has_content)
    {
        return;
    }
    if (has_rfi)
    {
        if (a->state == TAD_ASKER_TERMINAL_SETUP)
        {
            set_state(a, TAD_ASKER_RUNNING, 0, now_ms);
        }
        /* The grant stands even if our last text is still waiting for its ACK; write() takes
         * new text only once that ACK is in. */
        set_rfi(a, true);
        return;
    }
    enqueue(a, has_cesc ? TAD_SEND_CERS : TAD_SEND_DUMM, now_ms);
}

static void on_accept_reply(TadAsker *a, const XmsgDatagram *dg, uint32_t now_ms)
{
    XroutMessage m;

    if (xrout_parse(dg->body, dg->body_len, &m) != XMSG_OK)
    {
        XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_ERROR, "reply to the connect letter does not parse");
        fail(a, XMSG_EPROTO, now_ms);
        return;
    }
    if (!xrout_reply_accepted(&m))
    {
        XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_ERROR, "connect refused: XROUT error %u, %s", m.service,
                 xrout_error_name(m.service));
        fail(a, m.service, now_ms);
        return;
    }
    a->tadadm_port = dg->sub.src_port;
    set_state(a, TAD_ASKER_ACCEPTED, 0, now_ms);
    enqueue(a, TAD_SEND_SETUP, now_ms);
}

static void on_port_assignment(TadAsker *a, const uint8_t *chain, size_t clen, uint32_t now_ms)
{
    TadReader r;
    TadMessage m;
    bool got_port = false;

    tad_reader_init(&r, chain, clen);
    while (tad_reader_next(&r, &m))
    {
        if (m.opcode == TAD_OP_RAW_07 && m.len >= 5)
        {
            /* MEASURED in all 9 captured port assignments: byte 0 is 00, bytes 1-2 the host
             * system, bytes 3-4 the port every later host frame comes from. */
            a->session_port = xmsg_be16_read(m.data + 3);
            got_port = true;
        }
        else if (m.opcode == TAD_OP_LUN && m.len >= 2)
        {
            a->lun[0] = m.data[0];
            a->lun[1] = m.data[1];
        }
    }
    if (!got_port)
    {
        XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_WARN, "frame after the accept carries no port assignment");
        return;
    }
    XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_INFO, "session port 0x%04X, unit data %02X %02X",
             a->session_port, a->lun[0], a->lun[1]);
    set_state(a, TAD_ASKER_PORT_ASSIGNED, 0, now_ms);
}

static void asker_on_datagram(XmsgSession *s, const XmsgDatagram *dg, uint32_t now_ms)
{
    TadAsker *a = (TadAsker *)s;
    uint16_t lead;
    const uint8_t *chain;
    size_t clen;

    if (dg->repeat)
    {
        a->stats.repeats_in++; /* the node ACKed it again; we answered it the first time */
        return;
    }
    a->stats.frames_in++;
    if (a->state == TAD_ASKER_CONNECTING)
    {
        on_accept_reply(a, dg, now_ms);
        return;
    }
    if (tad_body_split(dg->body, dg->body_len, &lead, &chain, &clen) != XMSG_OK)
    {
        XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_WARN, "host frame of %u bytes is too short",
                 (unsigned)dg->body_len);
        return;
    }
    switch (a->state)
    {
    case TAD_ASKER_ACCEPTED:
        on_port_assignment(a, chain, clen, now_ms);
        break;
    case TAD_ASKER_PORT_ASSIGNED:
        if (dg->sub.src_port == a->session_port && tad_chain_has(chain, clen, TAD_OP_DUMM))
        {
            /* The host's first frame on the session port: now the terminal setup. */
            set_state(a, TAD_ASKER_TERMINAL_SETUP, 0, now_ms);
            enqueue(a, TAD_SEND_TERMINAL_SETUP, now_ms);
            enqueue(a, TAD_SEND_ESCA, now_ms);
        }
        break;
    case TAD_ASKER_TERMINAL_SETUP:
    case TAD_ASKER_RUNNING:
        answer_terminal_frame(a, chain, clen, now_ms);
        break;
    default:
        break;
    }
}

static const XmsgSessionOps s_asker_ops = {
    .on_datagram = asker_on_datagram,
    .poll_tx = asker_poll_tx,
    .tick = asker_tick,
    .on_delivered = asker_on_delivered,
    .on_error = asker_on_error,
    .on_peer_restart = asker_on_peer_restart,
};

/* ------------------------------------------------------------------------------------------ */
/* Public API                                                                                 */
/* ------------------------------------------------------------------------------------------ */

void tad_asker_config_default(TadAskerConfig *cfg, uint16_t peer_node, const char *system_name)
{
    if (cfg == NULL)
    {
        return;
    }
    memset(cfg, 0, sizeof *cfg);
    cfg->peer_node = peer_node;
    cfg->peer_system = peer_node;
    cfg->system_name = system_name;
    cfg->tmod = 0x08;
    cfg->ttyp[0] = 0x00;
    cfg->ttyp[1] = 0x00;
    cfg->desc = 0x1B;
    cfg->opsv[0] = 0x4C;
    cfg->opsv[1] = 0x01;
    cfg->opsv[2] = 0x04;
    cfg->keystroke_parity = true;
    cfg->connect_timeout_ms = 30000;
    cfg->ack_timeout_ms = 10000;
}

int tad_asker_init(TadAsker *a, const TadAskerConfig *cfg, const TadAskerCallbacks *cb)
{
    size_t name_len;

    if (a == NULL || cfg == NULL || cfg->system_name == NULL)
    {
        return XMSG_EINVAL;
    }
    name_len = strlen(cfg->system_name);
    if (name_len == 0 || name_len >= sizeof a->system_name)
    {
        return XMSG_EINVAL;
    }
    memset(a, 0, sizeof *a);
    a->cfg = *cfg;
    memcpy(a->system_name, cfg->system_name, name_len + 1);
    a->cfg.system_name = a->system_name;
    if (cb != NULL)
    {
        a->cb = *cb;
    }
    a->base.ops = &s_asker_ops;
    a->state = TAD_ASKER_IDLE;
    return XMSG_OK;
}

int tad_asker_connect(TadAsker *a, XmsgNode *node, uint32_t now_ms)
{
    int rc;

    if (a == NULL || node == NULL)
    {
        return XMSG_EINVAL;
    }
    if (a->state != TAD_ASKER_IDLE && a->state != TAD_ASKER_CLOSED && a->state != TAD_ASKER_FAILED)
    {
        return XMSG_ESTATE;
    }
    if (!xmsg_node_is_ready(node))
    {
        return XMSG_ESTATE;
    }
    a->base.peer_node = a->cfg.peer_node;
    a->base.peer_system = (a->cfg.peer_system != 0) ? a->cfg.peer_system : a->cfg.peer_node;
    /* The host answers from two ports (the *TADADM port and the session port), so the node
     * must not filter on one: this session's own port word already makes the match unique. */
    a->base.peer_port = 0;
    a->base.any_peer_port = true;
    a->q_head = 0;
    a->q_count = 0;
    a->inflight = TAD_SEND_NONE;
    a->xense_tries = 0;
    a->line_len = 0;
    a->rfi_open = false;
    a->tadadm_port = 0;
    a->session_port = 0;
    a->error = 0;
    rc = xmsg_node_attach(node, &a->base);
    if (rc != XMSG_OK)
    {
        return rc;
    }
    a->state = TAD_ASKER_IDLE;
    set_state(a, TAD_ASKER_CONNECTING, 0, now_ms);
    enqueue(a, TAD_SEND_LETTER, now_ms);
    xmsg_node_tick(node, now_ms); /* let the pump take the letter now */
    return XMSG_OK;
}

int tad_asker_write(TadAsker *a, const uint8_t *bytes, size_t len, uint32_t now_ms)
{
    size_t take;

    if (a == NULL || bytes == NULL || len == 0)
    {
        return XMSG_EINVAL;
    }
    if (a->state != TAD_ASKER_RUNNING)
    {
        return XMSG_ESTATE;
    }
    if (!a->rfi_open || a->line_len != 0)
    {
        return XMSG_EAGAIN;
    }
    take = (len > TAD_MAX_DATA) ? TAD_MAX_DATA : len;
    for (size_t i = 0; i < take; i++)
    {
        a->line[i] = a->cfg.keystroke_parity ? tad_even_parity(bytes[i]) : bytes[i];
    }
    a->line_len = (uint8_t)take;
    set_rfi(a, false); /* one BDAT per grant */
    enqueue(a, TAD_SEND_TEXT, now_ms);
    if (a->state == TAD_ASKER_RUNNING && a->base.node != NULL)
    {
        xmsg_node_tick(a->base.node, now_ms);
    }
    return (int)take;
}

bool tad_asker_may_type(const TadAsker *a)
{
    return a != NULL && a->state == TAD_ASKER_RUNNING && a->rfi_open && a->line_len == 0;
}

TadAskerState tad_asker_state(const TadAsker *a)
{
    return (a == NULL) ? TAD_ASKER_IDLE : a->state;
}

int tad_asker_hangup(TadAsker *a, uint32_t now_ms)
{
    if (a == NULL)
    {
        return XMSG_EINVAL;
    }
    if (a->state != TAD_ASKER_PORT_ASSIGNED && a->state != TAD_ASKER_TERMINAL_SETUP &&
        a->state != TAD_ASKER_RUNNING)
    {
        return XMSG_ESTATE;
    }
    set_rfi(a, false);
    set_state(a, TAD_ASKER_CLOSING, 0, now_ms);
    enqueue(a, TAD_SEND_DCON, now_ms);
    return XMSG_OK;
}

void tad_asker_abort(TadAsker *a)
{
    if (a == NULL || a->state == TAD_ASKER_IDLE || a->state == TAD_ASKER_CLOSED ||
        a->state == TAD_ASKER_FAILED)
    {
        return;
    }
    XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_WARN,
             "asker dropped locally in state %s; nothing sent, the host keeps its side",
             tad_asker_state_name(a->state));
    set_state(a, TAD_ASKER_CLOSED, 0, a->state_since_ms);
}

const char *tad_asker_state_name(TadAskerState s)
{
    switch (s)
    {
    case TAD_ASKER_IDLE:
        return "idle";
    case TAD_ASKER_CONNECTING:
        return "connecting";
    case TAD_ASKER_ACCEPTED:
        return "accepted";
    case TAD_ASKER_PORT_ASSIGNED:
        return "port-assigned";
    case TAD_ASKER_TERMINAL_SETUP:
        return "terminal-setup";
    case TAD_ASKER_RUNNING:
        return "running";
    case TAD_ASKER_CLOSING:
        return "closing";
    case TAD_ASKER_CLOSED:
        return "closed";
    case TAD_ASKER_FAILED:
        return "failed";
    default:
        return "?";
    }
}
