/*
 * lapb.c - the ND dialect of LAPB, as a pull-based state machine.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Behaviour follows RetroIO.Lapb LapbLayer.cs event for event; the header explains the measured
 * deviations from textbook LAPB and why each exists. Where this file differs from the C# it is
 * only in HOW frames leave: they are queued and pulled, never pushed through a callback, which is
 * what a non-blocking transport on a microcontroller needs.
 */
#include "libxmsg/lapb/lapb.h"

#include <string.h>

#include "libxmsg/log/xmsg_log.h"

#define NO_SLOT 0xFFu

/* ------------------------------------------------------------------------------------------ */
/* Small helpers                                                                              */
/* ------------------------------------------------------------------------------------------ */

const char *lapb_state_name(LapbState state)
{
    switch (state)
    {
    case LAPB_DISCONNECTED:
        return "DISCONNECTED";
    case LAPB_SABM_SENT:
        return "SABM_SENT";
    case LAPB_CONNECTED:
        return "CONNECTED";
    case LAPB_DISC_SENT:
        return "DISC_SENT";
    case LAPB_FRMR_SENT:
        return "FRMR_SENT";
    default:
        return "?";
    }
}

static uint8_t mod8(int v)
{
    return (uint8_t)(v & 0x07);
}

/* Signed "deadline reached" that survives the 32-bit millisecond counter wrapping. */
static bool time_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static void start_t1(LapbLink *l, uint32_t now)
{
    l->t1_running = true;
    l->t1_deadline = now + l->cfg.t1_ms;
}

static void stop_t1(LapbLink *l)
{
    l->t1_running = false;
}

static void start_t3(LapbLink *l, uint32_t now)
{
    l->t3_running = true;
    l->t3_deadline = now + l->cfg.t3_ms;
}

static void stop_t3(LapbLink *l)
{
    l->t3_running = false;
}

/* Tell the owner about a state change, and about the link becoming usable or not. */
static void notify_state(LapbLink *l, const char *reason)
{
    bool up = lapb_is_up(l);
    XMSG_LOG(XMSG_LOG_LAPB, XMSG_LOG_DEBUG, "state %s up=%d: %s", lapb_state_name(l->state),
             up ? 1 : 0, reason);
    l->was_up = up;
    if (l->on_state != NULL)
    {
        l->on_state(l->cb_ctx, l->state, up, reason);
    }
}

static void set_state(LapbLink *l, LapbState state, const char *reason)
{
    l->state = state;
    notify_state(l, reason);
}

/*
 * "Reset" in the spec's sense (section 8): V(S) = V(A) = V(R) = 0 and the retransmit window is
 * cleared. Frames committed but not yet pulled are lost with it, as in the C# (its pending queue
 * is cleared too; the comment there records that as a deliberate, open question). The owner is
 * told through on_state and must re-send what it still cares about.
 */
static void reset_variables(LapbLink *l)
{
    l->vs = 0;
    l->vr = 0;
    l->va = 0;
    for (int i = 0; i < 8; i++)
    {
        l->slots[i].len = 0;
        l->slots[i].due = false;
    }
    l->reserved_slot = NO_SLOT;
    l->peer_busy = false;
    l->reject_condition = false;
    l->poll_outstanding = false;
    l->poll_outstanding_marker = false;
    l->own_busy = false;
}

/* ------------------------------------------------------------------------------------------ */
/* Queuing control frames                                                                     */
/* ------------------------------------------------------------------------------------------ */

static void queue_ctl(LapbLink *l, const uint8_t *frame, size_t len)
{
    if (l->ctl_count >= LAPB_CTL_QUEUE_LEN)
    {
        l->stats.ctl_queue_full++;
        XMSG_LOG(XMSG_LOG_LAPB, XMSG_LOG_ERROR,
                 "control queue full (%u): frame dropped; pull more often", LAPB_CTL_QUEUE_LEN);
        return;
    }
    uint8_t at = (uint8_t)((l->ctl_head + l->ctl_count) % LAPB_CTL_QUEUE_LEN);
    memcpy(l->ctl_queue[at], frame, len);
    l->ctl_len[at] = (uint8_t)len;
    l->ctl_count++;
}

/* SABM, DISC, UA, DM: address 0x01, control, our node number (spec 2.3.1). */
static void queue_unnumbered(LapbLink *l, uint8_t u_type, bool pf)
{
    uint8_t f[4];
    f[0] = (uint8_t)LAPB_ADDR_LINK;
    f[1] = lapb_frame_u_control(u_type, pf);
    f[2] = (uint8_t)(l->cfg.own_node >> 8);
    f[3] = (uint8_t)(l->cfg.own_node & 0xFFu);
    queue_ctl(l, f, sizeof f);
}

/* RR, RNR, REJ: address 0x09, control with the CURRENT V(R), our node number. */
static void queue_supervisory(LapbLink *l, uint8_t s_type, bool pf)
{
    /* An RR with P=1 that WE originate (a T1 or T3 poll) leaves a poll outstanding: the next
     * S-frame with the P/F bit set is its final answer, not a poll of the peer's to be answered
     * in turn. The ND address byte has no command/response bit, so this flag is the only way to
     * tell the two apart; without it two stations answer each other's F=1 for ever. */
    if (pf && s_type == (uint8_t)LAPB_S_RR && l->poll_outstanding_marker)
    {
        l->poll_outstanding = true;
        l->poll_outstanding_marker = false;
    }
    uint8_t f[4];
    f[0] = lapb_frame_data_address(2);
    f[1] = lapb_frame_s_control(s_type, l->vr, pf);
    f[2] = (uint8_t)(l->cfg.own_node >> 8);
    f[3] = (uint8_t)(l->cfg.own_node & 0xFFu);
    queue_ctl(l, f, sizeof f);
}

/* The acknowledgement we send for received I-frames: RR normally, RNR while we are busy. */
static uint8_t ack_type(const LapbLink *l)
{
    return l->own_busy ? (uint8_t)LAPB_S_RNR : (uint8_t)LAPB_S_RR;
}

/* FRMR: address 0x01, control, the 3-byte diagnostic (spec 2.3.3). */
static void queue_frmr(LapbLink *l, bool pf)
{
    uint8_t f[5];
    f[0] = (uint8_t)LAPB_ADDR_LINK;
    f[1] = lapb_frame_u_control((uint8_t)LAPB_U_FRMR, pf);
    f[2] = l->frmr_info[0];
    f[3] = l->frmr_info[1];
    f[4] = l->frmr_info[2];
    queue_ctl(l, f, sizeof f);
    l->stats.frmr_sent++;
}

static void send_frmr(LapbLink *l, uint8_t rejected_control, uint8_t reason, bool pf, uint32_t now)
{
    /* Diagnostic: the rejected control byte; V(S) in bits 1..3 and V(R) in bits 5..7 with bit 4
     * (the C/R bit, which the ND dialect does not use) clear; the reason bits. */
    l->frmr_info[0] = rejected_control;
    l->frmr_info[1] = (uint8_t)(((l->vs << 1) & 0x0Eu) | ((l->vr << 5) & 0xE0u));
    l->frmr_info[2] = reason;
    l->frmr_valid = true;
    XMSG_LOG(XMSG_LOG_LAPB, XMSG_LOG_WARN, "FRMR: control 0x%02X reason 0x%02X", rejected_control,
             reason);
    queue_frmr(l, pf);
    set_state(l, LAPB_FRMR_SENT, "frame rejected");
    start_t1(l, now);
}

static void resend_frmr(LapbLink *l, uint32_t now)
{
    if (!l->frmr_valid)
    {
        return;
    }
    queue_frmr(l, true);
    start_t1(l, now);
}

/* ------------------------------------------------------------------------------------------ */
/* Public: set-up, connect, disconnect                                                        */
/* ------------------------------------------------------------------------------------------ */

void lapb_config_default(LapbConfig *cfg, uint16_t own_node)
{
    cfg->own_node = own_node;
    cfg->t1_ms = 3000;
    cfg->t3_ms = 30000;
    cfg->n2 = 10;
    cfg->window = LAPB_MAX_WINDOW;
    cfg->n1 = XMSG_LAPB_N1;
    cfg->reestablish_on_failure = false;
}

int lapb_init(LapbLink *l, const LapbConfig *cfg, void *cb_ctx, LapbInfoCallback on_info,
              LapbStateCallback on_state)
{
    if (l == NULL || cfg == NULL || cfg->window < 1 || cfg->window > LAPB_MAX_WINDOW ||
        cfg->n1 < 1 || cfg->n1 > XMSG_LAPB_N1)
    {
        return XMSG_EINVAL;
    }
    memset(l, 0, sizeof *l);
    l->cfg = *cfg;
    l->state = LAPB_DISCONNECTED;
    /* A station that never sends a SABM has no half of its own to wait for; lapb_connect()
     * clears this when a SABM goes out. */
    l->our_sabm_acked = true;
    l->peer_sabm_answered = false;
    l->peer_node = -1;
    l->reserved_slot = NO_SLOT;
    l->on_info = on_info;
    l->on_state = on_state;
    l->cb_ctx = cb_ctx;
    return XMSG_OK;
}

int lapb_connect(LapbLink *l, uint32_t now_ms)
{
    /* Our direction is now something to wait for; only the UA answering this SABM clears it.
     * The peer's half is untouched: if it already answered our earlier SABM, that stands. */
    l->our_sabm_acked = false;
    reset_variables(l);
    l->retry = 0;
    queue_unnumbered(l, (uint8_t)LAPB_U_SABM, true);
    set_state(l, LAPB_SABM_SENT, "connect: SABM sent");
    start_t1(l, now_ms);
    stop_t3(l);
    return XMSG_OK;
}

int lapb_disconnect(LapbLink *l, uint32_t now_ms)
{
    if (l->state == LAPB_DISCONNECTED || l->state == LAPB_DISC_SENT)
    {
        return XMSG_OK;
    }
    l->retry = 0;
    queue_unnumbered(l, (uint8_t)LAPB_U_DISC, true);
    set_state(l, LAPB_DISC_SENT, "disconnect: DISC sent");
    start_t1(l, now_ms);
    stop_t3(l);
    return XMSG_OK;
}

void lapb_force_down(LapbLink *l)
{
    reset_variables(l);
    l->ctl_count = 0;
    stop_t1(l);
    stop_t3(l);
    l->our_sabm_acked = true;
    l->peer_sabm_answered = false;
    set_state(l, LAPB_DISCONNECTED, "transport lost");
}

bool lapb_is_up(const LapbLink *l)
{
    return l->state == LAPB_CONNECTED && l->our_sabm_acked && l->peer_sabm_answered;
}

LapbState lapb_state(const LapbLink *l)
{
    return l->state;
}

int32_t lapb_peer_node(const LapbLink *l)
{
    return l->peer_node;
}

int lapb_outstanding(const LapbLink *l)
{
    return (int)mod8((int)l->vs - (int)l->va);
}

/* ------------------------------------------------------------------------------------------ */
/* Receiving                                                                                  */
/* ------------------------------------------------------------------------------------------ */

static void capture_node(LapbLink *l, const LapbFrameView *v)
{
    /* U and S frames carry the sender's node number (spec 2.3.1 / 3.3). Stored read-only as the
     * neighbour's id; it is never used as a routing key. */
    if (v->info_len >= 2)
    {
        l->peer_node = (int32_t)(((uint16_t)v->info[0] << 8) | v->info[1]);
    }
}

/* Mark every slot from `from` up to (not including) V(S) due again: go-back-N. */
static void mark_due_from(LapbLink *l, uint8_t from)
{
    uint8_t s = from;
    while (s != l->vs)
    {
        if (l->slots[s].len != 0)
        {
            l->slots[s].due = true;
            l->stats.retransmissions++;
        }
        s = mod8(s + 1);
    }
}

/*
 * Apply an N(R): it must lie in [V(A), V(S)] (spec 4.3) or it acknowledges a frame never sent,
 * which draws FRMR(Z). Returns false when the frame was rejected.
 */
static bool acknowledge_through(LapbLink *l, uint8_t nr, uint8_t rejected_control, uint32_t now)
{
    int outstanding = lapb_outstanding(l);
    if ((int)mod8((int)nr - (int)l->va) > outstanding)
    {
        send_frmr(l, rejected_control, (uint8_t)LAPB_FRMR_Z, false, now);
        return false;
    }
    while (l->va != nr)
    {
        l->slots[l->va].len = 0;
        l->slots[l->va].due = false;
        l->va = mod8(l->va + 1);
    }
    if (l->va == l->vs)
    {
        /* Everything acknowledged: stop retransmitting, start the idle keepalive. */
        stop_t1(l);
        start_t3(l, now);
    }
    else
    {
        start_t1(l, now);
    }
    l->retry = 0;
    return true;
}

static bool is_duplicate(const LapbLink *l, uint8_t ns)
{
    /* A gap is the peer running ahead of V(R) by 1..(k-1); at or beyond k round the modulo-8
     * circle the frame is already behind V(R): a retransmitted duplicate (spec 4.5). With k=7 the
     * common "one behind" retransmit (ahead == 7) is a duplicate that draws an RR, not a REJ. */
    return mod8((int)ns - (int)l->vr) >= l->cfg.window;
}

static void handle_unnumbered(LapbLink *l, const LapbFrameView *v, uint32_t now)
{
    switch (v->u_type)
    {
    case LAPB_U_SABM:
        /*
         * A SABM in ANY state (spec 6.3): record the peer's node, hard-reset the variables and
         * the window (no adoption of the peer's numbers), enter CONNECTED, and only THEN queue
         * the UA and the unsolicited RR. A SABM received mid-session is a legitimate reset and
         * the link stays up. This completes the PEER's half of the bring-up only.
         */
        capture_node(l, v);
        l->peer_sabm_answered = true;
        reset_variables(l);
        stop_t1(l);
        set_state(l, LAPB_CONNECTED, "SABM received");
        start_t3(l, now);
        queue_unnumbered(l, (uint8_t)LAPB_U_UA, true);
        queue_supervisory(l, (uint8_t)LAPB_S_RR, false);
        break;

    case LAPB_U_UA:
        capture_node(l, v);
        /* OUR half completes here whatever the state is (measured: the peer's SABM moved us to
         * CONNECTED 193 ms before the UA to our own SABM arrived). */
        l->our_sabm_acked = true;
        if (l->state == LAPB_SABM_SENT)
        {
            reset_variables(l);
            stop_t1(l);
            set_state(l, LAPB_CONNECTED, "UA received");
            start_t3(l, now);
            queue_supervisory(l, (uint8_t)LAPB_S_RR, false);
        }
        else if (l->state == LAPB_DISC_SENT)
        {
            stop_t1(l);
            stop_t3(l);
            set_state(l, LAPB_DISCONNECTED, "DISC acknowledged");
        }
        else
        {
            /* Already connected through the peer's SABM: just tell the owner we are now up. */
            notify_state(l, "late UA completed our half");
        }
        break;

    case LAPB_U_DISC:
        capture_node(l, v);
        if (l->state == LAPB_CONNECTED || l->state == LAPB_FRMR_SENT)
        {
            queue_unnumbered(l, (uint8_t)LAPB_U_UA, v->pf);
            stop_t1(l);
            stop_t3(l);
            set_state(l, LAPB_DISCONNECTED, "DISC received");
        }
        else
        {
            queue_unnumbered(l, (uint8_t)LAPB_U_DM, v->pf);
        }
        break;

    case LAPB_U_DM:
        capture_node(l, v);
        /* A DM tears the link down whether it answers our SABM or arrives while connected. */
        if (l->state == LAPB_SABM_SENT || l->state == LAPB_CONNECTED)
        {
            stop_t1(l);
            stop_t3(l);
            set_state(l, LAPB_DISCONNECTED, "DM received");
        }
        break;

    case LAPB_U_FRMR:
        l->stats.frmr_received++;
        XMSG_LOG(XMSG_LOG_LAPB, XMSG_LOG_WARN, "FRMR received: %02X %02X %02X",
                 v->info_len > 0 ? v->info[0] : 0, v->info_len > 1 ? v->info[1] : 0,
                 v->info_len > 2 ? v->info[2] : 0);
        /* The peer rejected one of our frames: re-establish with a fresh SABM. */
        if (l->state == LAPB_CONNECTED)
        {
            l->our_sabm_acked = false;
            reset_variables(l);
            queue_unnumbered(l, (uint8_t)LAPB_U_SABM, true);
            l->retry = 0;
            set_state(l, LAPB_SABM_SENT, "FRMR received: re-establishing");
            start_t1(l, now);
            stop_t3(l);
        }
        break;

    default:
        /* An unnumbered pattern we do not know (SARM, UI, ...): FRMR reason W. */
        send_frmr(l, v->control, (uint8_t)LAPB_FRMR_W, v->pf, now);
        break;
    }
}

static void handle_supervisory(LapbLink *l, const LapbFrameView *v, uint32_t now)
{
    capture_node(l, v);
    if (!acknowledge_through(l, v->nr, v->control, now))
    {
        return;
    }
    switch (v->s_type)
    {
    case LAPB_S_RR:
        l->peer_busy = false;
        break;
    case LAPB_S_RNR:
        l->peer_busy = true;
        break;
    case LAPB_S_REJ:
        l->stats.rej_received++;
        l->peer_busy = false;
        /* Go back N: everything from N(R) up to V(S) is due again. acknowledge_through() has
         * already moved V(A) to N(R). */
        mark_due_from(l, v->nr);
        start_t1(l, now);
        break;
    default:
        send_frmr(l, v->control, (uint8_t)LAPB_FRMR_W, v->pf, now);
        return;
    }
    if (v->pf)
    {
        if (l->poll_outstanding)
        {
            /* The final answer to a poll of ours. It is not answered again, and it is the
             * checkpoint of LAPB timer recovery: whatever its N(R) did not acknowledge was lost on
             * the way and is retransmitted now. Without this a lost LAST frame of a burst is never
             * recovered - the peer sees no gap, so it never sends REJ - and both stations poll each
             * other for ever (found with two stations on a lossy test wire, 2026-11-08). The C#
             * reference lacks this step and could only end such a stall in N2 failure. */
            l->poll_outstanding = false;
            if (l->va != l->vs)
            {
                mark_due_from(l, l->va);
                start_t1(l, now);
            }
        }
        else
        {
            /* A command with P set is answered with an RR carrying F=1 and the current N(R)
             * (X.25 2.4.5.9, ND spec 4.8). Deviation from the C# reference, which answers EVERY
             * S-frame with the bit set: against a real ND machine that only cost one harmless
             * extra RR per poll of ours, but between two of these stations it is an endless
             * exchange. Unverified against a real machine whether skipping that RR changes
             * anything; the P2 integration test watches for it. */
            queue_supervisory(l, ack_type(l), true);
        }
    }
}

static void handle_information(LapbLink *l, const LapbFrameView *v, uint32_t now)
{
    if (!acknowledge_through(l, v->nr, v->control, now))
    {
        return;
    }
    if (v->info_len > l->cfg.n1)
    {
        send_frmr(l, v->control, (uint8_t)LAPB_FRMR_Y, v->pf, now);
        return;
    }
    if (v->ns == l->vr)
    {
        /* In sequence: deliver, advance V(R), clear any reject condition, acknowledge (4.4). */
        l->vr = mod8(l->vr + 1);
        l->reject_condition = false;
        l->stats.info_delivered++;
        if (l->on_info != NULL)
        {
            l->on_info(l->cb_ctx, v->info, v->info_len);
        }
        queue_supervisory(l, ack_type(l), v->pf);
    }
    else if (is_duplicate(l, v->ns))
    {
        /* Already accepted and retransmitted: discard, re-acknowledge V(R) (4.5). */
        l->stats.duplicates++;
        queue_supervisory(l, ack_type(l), v->pf);
    }
    else if (!l->reject_condition)
    {
        /* A gap: exactly one REJ, then silence until the gap is filled (4.5). */
        queue_supervisory(l, (uint8_t)LAPB_S_REJ, v->pf);
        l->stats.rej_sent++;
        l->reject_condition = true;
    }
}

int lapb_rx_frame(LapbLink *l, const uint8_t *frame, size_t len, uint32_t now_ms)
{
    LapbFrameView v;

    l->stats.frames_in++;
    if (!lapb_frame_parse(frame, len, &v))
    {
        return XMSG_EPROTO;
    }
    if (!lapb_frame_address_known(v.address))
    {
        l->stats.bad_address++;
        XMSG_LOG(XMSG_LOG_LAPB, XMSG_LOG_DEBUG, "frame with address 0x%02X ignored", v.address);
        return XMSG_EPROTO;
    }

    if (v.kind == LAPB_FRAME_UNNUMBERED)
    {
        handle_unnumbered(l, &v, now_ms);
        return XMSG_OK;
    }

    /* I- and S-frames are processed only in CONNECTED (spec 6.3). */
    switch (l->state)
    {
    case LAPB_DISCONNECTED:
        queue_unnumbered(l, (uint8_t)LAPB_U_DM, v.pf);
        return XMSG_OK;
    case LAPB_SABM_SENT:
    case LAPB_DISC_SENT:
        return XMSG_OK;
    case LAPB_FRMR_SENT:
        resend_frmr(l, now_ms);
        return XMSG_OK;
    default:
        break;
    }

    if (v.kind == LAPB_FRAME_INFORMATION)
    {
        handle_information(l, &v, now_ms);
    }
    else
    {
        handle_supervisory(l, &v, now_ms);
    }
    return XMSG_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* Sending                                                                                    */
/* ------------------------------------------------------------------------------------------ */

int lapb_tx_free_slots(const LapbLink *l)
{
    if (!lapb_is_up(l))
    {
        return 0;
    }
    int used = lapb_outstanding(l) + ((l->reserved_slot != NO_SLOT) ? 1 : 0);
    return (int)l->cfg.window - used;
}

int lapb_tx_reserve(LapbLink *l, uint8_t **info, size_t *cap)
{
    if (!lapb_is_up(l))
    {
        return XMSG_ESTATE;
    }
    if (l->reserved_slot != NO_SLOT || lapb_outstanding(l) >= (int)l->cfg.window)
    {
        return XMSG_EAGAIN;
    }
    l->reserved_slot = l->vs;
    *info = l->slots[l->vs].info;
    *cap = l->cfg.n1;
    return XMSG_OK;
}

void lapb_tx_cancel(LapbLink *l)
{
    l->reserved_slot = NO_SLOT;
}

int lapb_tx_commit(LapbLink *l, size_t len, uint32_t now_ms)
{
    if (l->reserved_slot == NO_SLOT)
    {
        return XMSG_ESTATE;
    }
    if (len == 0 || len > l->cfg.n1)
    {
        return XMSG_EINVAL;
    }
    LapbSlot *s = &l->slots[l->reserved_slot];
    s->len = (uint16_t)len;
    s->due = true;
    l->reserved_slot = NO_SLOT;
    /* V(S) advances now, before the frame leaves: an acknowledgement that arrives inside the
     * transmit path (loopback, in-process pair) must find the frame counted. */
    l->vs = mod8(l->vs + 1);
    /* An I-frame is outstanding: the idle keepalive stops, T1 runs if it was idle (spec 5.1). */
    stop_t3(l);
    if (!l->t1_running)
    {
        start_t1(l, now_ms);
    }
    return XMSG_OK;
}

bool lapb_tx_pending(const LapbLink *l)
{
    if (l->ctl_count > 0)
    {
        return true;
    }
    if (l->state != LAPB_CONNECTED || l->peer_busy)
    {
        return false;
    }
    uint8_t s = l->va;
    while (s != l->vs)
    {
        if (l->slots[s].due)
        {
            return true;
        }
        s = mod8(s + 1);
    }
    return false;
}

void lapb_set_busy(LapbLink *l, bool busy)
{
    if (l->own_busy == busy || l->state != LAPB_CONNECTED)
    {
        return;
    }
    l->own_busy = busy;
    XMSG_LOG(XMSG_LOG_LAPB, XMSG_LOG_DEBUG, "flow control: %s", busy ? "RNR (busy)" : "RR (ready)");
    queue_supervisory(l, busy ? (uint8_t)LAPB_S_RNR : (uint8_t)LAPB_S_RR, false);
}

int lapb_pull_tx(LapbLink *l, uint8_t *out, size_t cap)
{
    if (l->ctl_count > 0)
    {
        uint8_t len = l->ctl_len[l->ctl_head];
        if (cap < len)
        {
            return XMSG_ENOSPC;
        }
        memcpy(out, l->ctl_queue[l->ctl_head], len);
        l->ctl_head = (uint8_t)((l->ctl_head + 1) % LAPB_CTL_QUEUE_LEN);
        l->ctl_count--;
        l->stats.frames_out++;
        return (int)len;
    }
    if (l->state != LAPB_CONNECTED || l->peer_busy)
    {
        return 0;
    }
    /* Due I-frames in N(S) order from V(A): the control byte is built NOW so a retransmission
     * carries the current V(R) as its acknowledgement (spec 4.6). */
    uint8_t s = l->va;
    while (s != l->vs)
    {
        LapbSlot *slot = &l->slots[s];
        if (slot->due)
        {
            size_t len = 2u + slot->len;
            if (cap < len)
            {
                return XMSG_ENOSPC;
            }
            out[0] = lapb_frame_data_address(slot->len);
            out[1] = lapb_frame_i_control(s, l->vr, false);
            memcpy(out + 2, slot->info, slot->len);
            slot->due = false;
            l->stats.frames_out++;
            return (int)len;
        }
        s = mod8(s + 1);
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* Timers                                                                                     */
/* ------------------------------------------------------------------------------------------ */

static int on_t1_expiry(LapbLink *l, uint32_t now)
{
    l->stats.t1_timeouts++;
    l->retry++;
    if (l->retry > l->cfg.n2)
    {
        /*
         * N2 exhausted. Textbook LAPB re-establishes with a fresh SABM; against a real ND machine
         * that SABM killed the XMSG gateway (measured twice on D100, 2026-08-21, XMSG ERROR CODE
         * 27 within 200 ms, taking the file server and terminal server down with it). So by
         * default the link goes DOWN and the owner is told; reestablish_on_failure opts in to the
         * textbook behaviour for tests.
         */
        stop_t3(l);
        if (l->cfg.reestablish_on_failure &&
            (l->state == LAPB_CONNECTED || l->state == LAPB_SABM_SENT))
        {
            l->our_sabm_acked = false;
            reset_variables(l);
            queue_unnumbered(l, (uint8_t)LAPB_U_SABM, true);
            l->retry = 0;
            set_state(l, LAPB_SABM_SENT, "N2 exhausted: re-establishing (opt-in)");
            start_t1(l, now);
            return 1;
        }
        LapbState from = l->state;
        int outstanding = lapb_outstanding(l);
        stop_t1(l);
        if (from == LAPB_CONNECTED || from == LAPB_SABM_SENT)
        {
            XMSG_LOG(XMSG_LOG_LAPB, XMSG_LOG_ERROR,
                     "link failure: N2 (%u) exhausted in %s with %d unacknowledged frame(s); NOT "
                     "re-establishing (a SABM kills the peer's XMSG gateway)",
                     l->cfg.n2, lapb_state_name(from), outstanding);
            set_state(l, LAPB_DISCONNECTED, "N2 exhausted: link failure");
        }
        else
        {
            /* A DISC_SENT or FRMR_SENT timeout ending in DISCONNECTED is an ordinary teardown. */
            set_state(l, LAPB_DISCONNECTED, "N2 exhausted during teardown");
        }
        return 1;
    }

    switch (l->state)
    {
    case LAPB_CONNECTED:
        /* Poll-first recovery (spec 5.1): RR with P=1; the peer's N(R) or REJ drives go-back-N. */
        l->poll_outstanding_marker = true;
        queue_supervisory(l, ack_type(l), true);
        start_t1(l, now);
        return 1;
    case LAPB_SABM_SENT:
        queue_unnumbered(l, (uint8_t)LAPB_U_SABM, true);
        start_t1(l, now);
        return 1;
    case LAPB_DISC_SENT:
        queue_unnumbered(l, (uint8_t)LAPB_U_DISC, true);
        start_t1(l, now);
        return 1;
    case LAPB_FRMR_SENT:
        resend_frmr(l, now);
        return 1;
    default:
        stop_t1(l);
        return 0;
    }
}

int lapb_tick(LapbLink *l, uint32_t now_ms)
{
    int acted = 0;

    if (l->t1_running && time_reached(now_ms, l->t1_deadline))
    {
        acted = on_t1_expiry(l, now_ms);
        if (l->state == LAPB_DISCONNECTED)
        {
            return acted;
        }
    }
    if (l->t3_running && time_reached(now_ms, l->t3_deadline) && l->state == LAPB_CONNECTED)
    {
        /* Idle keepalive (spec 5.2): ONE RR with P=1, and T3 stops. T1 is NOT started: an
         * ignored idle poll must not turn into a re-poll cascade that reaches N2 and resets a
         * healthy link. T3 re-arms when the peer answers (acknowledge_through). */
        l->poll_outstanding_marker = true;
        queue_supervisory(l, ack_type(l), true);
        l->stats.t3_polls++;
        l->retry = 0;
        stop_t3(l);
        acted = 1;
    }
    return acted;
}
