/*
 * lapb.h - the ND dialect of LAPB: the link between two machines on an XMSG network.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * What this layer does: brings the link up (both sides send a SABM and answer the other's; the
 * link is usable only when BOTH exchanges are done), numbers the information frames modulo 8,
 * acknowledges, retransmits on REJ or timeout, honours RNR, polls an idle link every T3, and
 * takes the link down after N2 unanswered polls. Everything above it (the XMSG datagram layer)
 * sees a sequence of information fields in order, each delivered once.
 *
 * Where the behaviour comes from: the C# RetroIO.Lapb LapbLayer.cs, which was proven against
 * real ND-100 machines and whose comments record what each deviation from textbook LAPB cost
 * when it was wrong. The ones that matter most:
 *
 *  - On a received SABM: change state FIRST, then send UA, then an unsolicited RR. The RR is what
 *    moves the ND kernel's link to its RUN phase. (Measured; without the RR the peer sits in
 *    CONNECT for ever.)
 *  - A UA completes OUR half of the bring-up in ANY state: the peer's SABM may have moved us to
 *    CONNECTED 200 ms before the UA to our own SABM arrives (measured 2026-08-19). A link whose
 *    UA was only honoured in SABM_SENT stayed half-up for ever.
 *  - N2 exhaustion takes the link DOWN and does NOT send a fresh SABM by default. A re-SABM
 *    killed the XMSG gateway on D100 twice (XMSG ERROR CODE 27 within 200 ms), taking the file
 *    server and the terminal server with it. `reestablish_on_failure` opts back in to the
 *    textbook behaviour for tests.
 *  - A T3 keepalive is ONE RR with P=1 and it does not start T1. Starting T1 turned an ignored
 *    idle poll into a 3-second re-poll cascade that reached N2 and reset a healthy link.
 *  - A received SABM in mid-session is a legitimate reset, not an error: variables and the
 *    transmit window are cleared and the link STAYS up.
 *  - V(S) advances before the frame leaves, and the address byte is 0x89 for an odd information
 *    length (see lapb_frame.h).
 *
 * How sending works - pull, not push. A session that wants to send reserves one of the eight
 * window slots with lapb_tx_reserve(), builds its information field in place, and commits. The
 * slot is then "due". Whoever owns the transport (the node) calls lapb_pull_tx() to take the next
 * due frame - control frames first, then information frames in N(S) order - and hands it to the
 * framer. Nothing is copied twice and nothing is allocated. On REJ or a T1 poll answer the slots
 * are simply marked due again, with the CURRENT N(R) stamped at pull time, which is what the
 * spec wants a retransmission to carry.
 *
 * Time: every call that can start or expire a timer takes `now_ms`, a monotonic millisecond
 * count the caller owns. The layer never reads a clock. Call lapb_tick() at least every few
 * hundred milliseconds and after every received frame.
 *
 * Not implemented, on purpose: the ITU-T X.25 variant (0x03/0x01 addresses, no node number),
 * which no SINTRAN link uses, and the optional T2 acknowledgement delay, which the C# keeps off
 * against ND machines. Every acknowledgement goes out at once.
 */
#ifndef XMSG_LAPB_H
#define XMSG_LAPB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libxmsg/lapb/lapb_frame.h"
#include "libxmsg/xmsg_config.h"
#include "libxmsg/xmsg_errors.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Window size modulo 8 allows at most 7 outstanding frames. */
#define LAPB_MAX_WINDOW 7

/** Control frames queued and not yet pulled. A burst of 6 SABMs needs 12; this leaves room. */
#define LAPB_CTL_QUEUE_LEN 16

/** The longest control frame: address, control, and a 3-byte FRMR diagnostic. */
#define LAPB_CTL_FRAME_MAX 5

/** Link parameters. lapb_config_default() fills the measured ND defaults. */
typedef struct LapbConfig
{
    uint16_t own_node;           /**< our node number, sent in every U and S frame */
    uint32_t t1_ms;              /**< retransmission / poll timer; 3000 */
    uint32_t t3_ms;              /**< idle keepalive timer; 30000 */
    uint8_t n2;                  /**< retries before the link is declared failed; 10 */
    uint8_t window;              /**< outstanding I-frames allowed, 1..7; 7 */
    uint16_t n1;                 /**< longest information field accepted and sent; XMSG_LAPB_N1 */
    bool reestablish_on_failure; /**< send a fresh SABM after N2 (textbook; OFF: kills ND XMSG) */
} LapbConfig;

/** The link states of ND spec section 6. */
typedef enum LapbState
{
    LAPB_DISCONNECTED = 0,
    LAPB_SABM_SENT = 1,
    LAPB_CONNECTED = 2,
    LAPB_DISC_SENT = 3,
    LAPB_FRMR_SENT = 4
} LapbState;

/** One transmit window slot: the information field for one N(S). */
typedef struct LapbSlot
{
    uint16_t len;               /**< bytes in info; 0 when the slot is free */
    bool due;                   /**< waiting to be pulled (first send or retransmission) */
    uint8_t info[XMSG_LAPB_N1]; /**< the information field */
} LapbSlot;

/** Counters over the life of the link object; never reset by a reconnect. */
typedef struct LapbStats
{
    uint32_t frames_in;       /**< frames handed to lapb_rx_frame() */
    uint32_t frames_out;      /**< frames pulled */
    uint32_t info_delivered;  /**< information fields passed up, in order */
    uint32_t retransmissions; /**< I-frames marked due a second time */
    uint32_t rej_sent;
    uint32_t rej_received;
    uint32_t frmr_sent;
    uint32_t frmr_received;
    uint32_t t1_timeouts;
    uint32_t t3_polls;
    uint32_t duplicates;     /**< I-frames already received, re-acknowledged */
    uint32_t bad_address;    /**< frames with an address the ND dialect does not use */
    uint32_t ctl_queue_full; /**< control frames that could not be queued (should stay 0) */
} LapbStats;

/** Called with each in-sequence information field, in order, exactly once. */
typedef void (*LapbInfoCallback)(void *ctx, const uint8_t *info, size_t len);

/** Called on every state change with a short reason, and separately when the link becomes
 *  usable (both halves done) or stops being usable. `up` is lapb_is_up() after the change. */
typedef void (*LapbStateCallback)(void *ctx, LapbState state, bool up, const char *reason);

/** The link. Treat every field as private; use the functions. */
typedef struct LapbLink
{
    LapbConfig cfg;
    LapbState state;
    bool our_sabm_acked;          /**< our SABM has been answered (or we never sent one) */
    bool peer_sabm_answered;      /**< we have answered the peer's SABM */
    bool was_up;                  /**< lapb_is_up() at the last state callback */
    uint8_t vs;                   /**< V(S): next I-frame to send */
    uint8_t vr;                   /**< V(R): next in-sequence I-frame expected */
    uint8_t va;                   /**< V(A): oldest unacknowledged I-frame */
    bool peer_busy;               /**< RNR received, not yet cleared by RR/REJ */
    bool reject_condition;        /**< one REJ per gap */
    bool own_busy;                /**< we told the peer RNR; acks go out as RNR */
    bool poll_outstanding;        /**< we sent an RR P=1 and its F=1 answer has not arrived */
    bool poll_outstanding_marker; /**< internal: the next queued RR P=1 is a poll of ours */
    bool t1_running;
    bool t3_running;
    uint32_t t1_deadline;
    uint32_t t3_deadline;
    uint8_t retry;
    int32_t peer_node; /**< learned from the peer's frames; -1 until then; never a routing key */
    LapbSlot slots[8]; /**< indexed by N(S) */
    uint8_t ctl_queue[LAPB_CTL_QUEUE_LEN][LAPB_CTL_FRAME_MAX];
    uint8_t ctl_len[LAPB_CTL_QUEUE_LEN];
    uint8_t ctl_head;
    uint8_t ctl_count;
    uint8_t frmr_info[3]; /**< the last FRMR diagnostic, for resending in FRMR_SENT */
    bool frmr_valid;
    uint8_t reserved_slot; /**< slot handed out by lapb_tx_reserve(), or 0xFF */
    LapbStats stats;
    LapbInfoCallback on_info;
    LapbStateCallback on_state;
    void *cb_ctx;
} LapbLink;

/**
 * @brief Fill the measured ND defaults: T1 3000 ms, T3 30000 ms, N2 10, window 7, N1 XMSG_LAPB_N1,
 *        no re-establish on failure.
 * @param cfg      Receives the defaults.
 * @param own_node Our node number.
 */
void lapb_config_default(LapbConfig *cfg, uint16_t own_node);

/**
 * @brief Set up a link in DISCONNECTED.
 * @param l        The link.
 * @param cfg      Parameters; copied. window must be 1..7, n1 at most XMSG_LAPB_N1.
 * @param cb_ctx   Passed to both callbacks.
 * @param on_info  Receives information fields; may be NULL (they are then dropped and counted).
 * @param on_state Receives state changes; may be NULL.
 * @return XMSG_OK or XMSG_EINVAL.
 */
int lapb_init(LapbLink *l, const LapbConfig *cfg, void *cb_ctx, LapbInfoCallback on_info,
              LapbStateCallback on_state);

/**
 * @brief Start the link: queue a SABM with P=1 and our node number, enter SABM_SENT, start T1.
 * @param l      The link.
 * @param now_ms The time.
 * @return XMSG_OK.
 */
int lapb_connect(LapbLink *l, uint32_t now_ms);

/**
 * @brief Take the link down politely: queue a DISC, enter DISC_SENT. No-op when already down.
 * @param l      The link.
 * @param now_ms The time.
 * @return XMSG_OK.
 */
int lapb_disconnect(LapbLink *l, uint32_t now_ms);

/**
 * @brief The transport died: drop everything and go to DISCONNECTED without sending anything.
 * @param l The link.
 */
void lapb_force_down(LapbLink *l);

/**
 * @brief Hand a received frame (address .. info, FCS removed and verified) to the link.
 * @param l      The link.
 * @param frame  Bytes.
 * @param len    Count.
 * @param now_ms The time.
 * @return XMSG_OK; XMSG_EPROTO for a frame too short to have a control byte or with an address
 *         the ND dialect does not use (counted, otherwise ignored).
 */
int lapb_rx_frame(LapbLink *l, const uint8_t *frame, size_t len, uint32_t now_ms);

/**
 * @brief Whether information may be sent: CONNECTED and both bring-up halves done.
 * @param l The link.
 * @return true when up.
 */
bool lapb_is_up(const LapbLink *l);

/** @brief The current state. */
LapbState lapb_state(const LapbLink *l);

/** @brief The peer's node number as learned from its frames, or -1. */
int32_t lapb_peer_node(const LapbLink *l);

/** @brief Outstanding (sent, unacknowledged) I-frames. */
int lapb_outstanding(const LapbLink *l);

/** @brief Window slots free for lapb_tx_reserve(). 0 while the link is not up. */
int lapb_tx_free_slots(const LapbLink *l);

/**
 * @brief Reserve the next window slot and get its buffer to build an information field in.
 *
 * Exactly one reservation may be open at a time; commit it or cancel it before the next.
 *
 * @param l    The link.
 * @param info Receives a pointer to cfg.n1 bytes to fill.
 * @param cap  Receives cfg.n1.
 * @return XMSG_OK; XMSG_ESTATE when the link is not up; XMSG_EAGAIN when the window is full or a
 *         reservation is already open.
 */
int lapb_tx_reserve(LapbLink *l, uint8_t **info, size_t *cap);

/**
 * @brief Commit the reserved slot: it becomes I-frame N(S) = V(S), V(S) advances, the frame is due.
 * @param l      The link.
 * @param len    Bytes written into the slot; 1..cfg.n1.
 * @param now_ms The time (T1 starts if it was idle, T3 stops).
 * @return XMSG_OK; XMSG_EINVAL for a bad length; XMSG_ESTATE with no open reservation.
 */
int lapb_tx_commit(LapbLink *l, size_t len, uint32_t now_ms);

/**
 * @brief Give up an open reservation without sending.
 * @param l The link.
 */
void lapb_tx_cancel(LapbLink *l);

/**
 * @brief Take the next frame to transmit: control frames in queue order first, then due I-frames
 *        in N(S) order (held back while the peer is busy).
 * @param l   The link.
 * @param out Receives address, control and information; needs cfg.n1 + 2 bytes.
 * @param cap Its size.
 * @return The frame length, 0 when nothing is due, or XMSG_ENOSPC when `cap` is too small for the
 *         next frame (nothing is consumed).
 */
int lapb_pull_tx(LapbLink *l, uint8_t *out, size_t cap);

/**
 * @brief Whether lapb_pull_tx() would return a frame.
 * @param l The link.
 */
bool lapb_tx_pending(const LapbLink *l);

/**
 * @brief Flow control towards the peer: tell it to stop sending I-frames (busy) or resume.
 *
 * Sends RNR when becoming busy and RR when clearing it; while busy every acknowledgement goes out
 * as RNR instead of RR, so the peer keeps seeing the condition. The layer above uses this when it
 * cannot absorb more datagrams - for instance when its queue of acknowledgements to send is
 * filling because its own window is stuck behind lost frames.
 *
 * @param l    The link.
 * @param busy true to stop the peer, false to release it. A no-op when nothing changes or the
 *             link is not connected.
 */
void lapb_set_busy(LapbLink *l, bool busy);

/**
 * @brief Run the timers.
 * @param l      The link.
 * @param now_ms The time.
 * @return 1 when a timer fired and something was queued or the state changed, else 0.
 */
int lapb_tick(LapbLink *l, uint32_t now_ms);

/** @brief Plain-words name of a state, for logs and tools. */
const char *lapb_state_name(LapbState state);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_LAPB_H */
