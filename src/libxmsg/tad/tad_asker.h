/*
 * tad_asker.h - a terminal session to a SINTRAN host: the side that types.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * What CONNECT-TO does on a real ND machine, reproduced frame for frame. The authority is the
 * capture of a real D100 asking a D102 host in RetroIO pcap-decode-sample.json (records 244-341
 * of that file). Every frame we send is pinned byte for byte against it by test_tad_asker.c.
 *
 *   asker -> port 0          letter naming "*TADADM" and the target system name
 *                            (frame flags 0x86, role 0xE4, control word 0x0400)
 *   host                     accept from the *TADADM port (we learn that port), or a refusal
 *                            whose service byte is the XROUT error code
 *   asker -> *TADADM port    session setup `06 00 1B 00 1C 01 00 FF 00` (0x86, 0x84, 0x0400)
 *   host                     port assignment: opcode 0x07 carries the host system in bytes 1-2
 *                            and the SESSION PORT (bytes 3-4), 0x0B the logical unit
 *   host                     DUMM from the session port
 *   asker -> session port    terminal setup TMOD 08, TTYP 0000, DESC 1B, OPSV 4C 01 04
 *                            (0x86, 0x84, 0x0108), then ESCA (0x82, 0x94, 0x0008)
 *   host                     ESRS, RESE, RESE, then the banner ending in RFI
 *   asker                    keystrokes as BDAT (0x96, 0x84, 0x0108) once input is granted
 *   host                     0xFD from the *TADADM port: the session is over
 *   asker -> session port    DCON (0x82, 0x94, 0x0008)
 *
 * How the asker answers each host terminal frame (MEASURED: the rule below predicts the asker's
 * control replies exactly, in order, in 9 of the 10 sessions in that capture file; the tenth has
 * no host frames in the file). One reply per host frame, decided by what the frame holds:
 *   - a RESE                                  -> RECO
 *   - only DUMM or only ESRS                  -> nothing
 *   - anything else that holds an RFI         -> nothing; the user may type
 *   - anything else that holds a CESC         -> CERS
 *   - any other content (text, ECKM, SYCN...) -> DUMM
 * The RetroIO C# asker answers RFI with CERS instead; the capture of the real machine says
 * otherwise, and this file follows the capture.
 *
 * Text, both ways:
 *  - host BDAT bytes are handed to text_out untouched. The captured host text is 7-bit, so
 *    whether a host ever sets bit 7 is not known; passing the bytes raw is the 8-bit clean
 *    choice SINTRAN's Kermit needs, and it hides nothing.
 *  - keystrokes go out with even parity in bit 7 when `keystroke_parity` is set (the default,
 *    MEASURED: "sys" + CR is sent as F3 F9 F3 8D). Clear it to send bytes as given.
 *  - echo: the host does NOT echo what is typed (after "sys" + CR the host sent only CR LF).
 *    ECKM tells the asker what to echo locally: 0x01 echo, 0xFF do not (a password prompt).
 *    The library reports ECKM; echoing is the application's job.
 *  - input: typing is allowed after an RFI and closes again with each BDAT sent, so
 *    tad_asker_write() takes at most 255 bytes per grant. MEASURED: every typed BDAT in the
 *    capture follows an RFI.
 *
 * Our own choices, not wire facts: the asker keeps ONE frame unacknowledged at a time, so a
 * frame the host refuses with XENSE is simply rebuilt and sent again at the stepped-down number
 * (the node steps the count down; up to 8 tries). Any other refusal ends the session.
 *
 * NOT ESTABLISHED, so not offered: what an asker sends to end a session on its own. The only
 * measured ending is the host's 0xFD (the user typed LOGOUT) answered with DCON.
 * tad_asker_abort() drops the session locally and sends nothing; the host keeps its side until
 * it gives up on its own.
 */
#ifndef XMSG_TAD_ASKER_H
#define XMSG_TAD_ASKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libxmsg/tad/tad_codec.h"
#include "libxmsg/xmsg/xmsg_node.h"
#include "libxmsg/xmsg/xmsg_session.h"
#include "libxmsg/xmsg_errors.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Control words (sub-header XMCSM, also the frame's Flags2) on the frames we send. */
#define TAD_XMCSM_LETTER   0x0400u /**< the connect letter */
#define TAD_XMCSM_SETUP    0x0400u /**< the session setup */
#define TAD_XMCSM_TERMINAL 0x0108u /**< terminal setup, BDAT, RECO, CERS, DUMM */
#define TAD_XMCSM_BARE     0x0008u /**< ESCA, DCON */

/* Role bytes (sub-header byte 3) on the frames we send. */
#define TAD_ROLE_LETTER  0xE4u /**< the connect letter */
#define TAD_ROLE_DATA    0x84u /**< setup frames and typed text */
#define TAD_ROLE_CONTROL 0x94u /**< ESCA, RECO, CERS, DUMM, DCON */

/** The name of the terminal service on every SINTRAN machine. */
#define TAD_SERVICE_NAME "*TADADM"

/** XENSE resend limit: the node gives up stepping down after this many. */
#define TAD_ASKER_XENSE_TRIES 8u

/** Pending replies the asker can hold; more means the host outran us, which fails loudly. */
#define TAD_ASKER_QUEUE 16u

typedef enum TadAskerState
{
    TAD_ASKER_IDLE = 0,
    TAD_ASKER_CONNECTING,     /**< letter sent, waiting for the accept */
    TAD_ASKER_ACCEPTED,       /**< session setup sent, waiting for the port assignment */
    TAD_ASKER_PORT_ASSIGNED,  /**< waiting for the host's DUMM on the session port */
    TAD_ASKER_TERMINAL_SETUP, /**< terminal setup and ESCA sent, waiting for the first RFI */
    TAD_ASKER_RUNNING,        /**< text flows */
    TAD_ASKER_CLOSING,        /**< the host ended the session; DCON being sent */
    TAD_ASKER_CLOSED,         /**< DCON acknowledged; detached from the node */
    TAD_ASKER_FAILED          /**< refused or timed out; detached from the node */
} TadAskerState;

/** What the session tells the application. Any pointer may be NULL. */
typedef struct TadAskerCallbacks
{
    void *ctx;
    /** Host text, exactly as received. */
    void (*text_out)(void *ctx, const uint8_t *bytes, size_t len);
    /** State changed. On TAD_ASKER_FAILED `error` is an XROUT code from a refusal (positive),
     *  an XMSG error such as XEIMA (negative, below -6), or XMSG_ETIMEDOUT; 0 otherwise. */
    void (*state)(void *ctx, TadAskerState state, int error);
    /** The host granted input (true) or the grant was used (false). */
    void (*input_gate)(void *ctx, bool may_type);
    /** The host's echo instruction: the ECKM byte (0x01 echo locally, 0xFF do not). */
    void (*echo_mode)(void *ctx, uint8_t eckm);
} TadAskerCallbacks;

/** Settings; tad_asker_config_default() fills the measured values. */
typedef struct TadAskerConfig
{
    uint16_t peer_node;          /**< LAPB node number of the host */
    uint16_t peer_system;        /**< its XMSG system number (0 = same as peer_node) */
    const char *system_name;     /**< the target system as XROUT knows it, e.g. "D102" */
    uint8_t tmod;                /**< terminal mode in the setup: 0x08 */
    uint8_t ttyp[2];             /**< terminal type: 00 00 */
    uint8_t desc;                /**< escape key: 0x1B */
    uint8_t opsv[3];             /**< OPSV: 4C 01 04 */
    bool keystroke_parity;       /**< even parity in bit 7 on typed bytes: true */
    uint32_t connect_timeout_ms; /**< how long to wait for the accept: 30000 */
    uint32_t ack_timeout_ms;     /**< how long to wait for the host's ACK of a frame: 10000 */
} TadAskerConfig;

/** The kinds of frame the asker sends. */
typedef enum TadAskerSend
{
    TAD_SEND_NONE = 0,
    TAD_SEND_LETTER,
    TAD_SEND_SETUP,
    TAD_SEND_TERMINAL_SETUP,
    TAD_SEND_ESCA,
    TAD_SEND_RECO,
    TAD_SEND_CERS,
    TAD_SEND_DUMM,
    TAD_SEND_TEXT,
    TAD_SEND_DCON
} TadAskerSend;

/** Counters for tests and diagnostics. */
typedef struct TadAskerStats
{
    uint32_t frames_in;
    uint32_t frames_out;
    uint32_t repeats_in; /**< host frames seen again (already answered once) */
    uint32_t xense_resends;
    uint32_t text_bytes_in;
    uint32_t text_bytes_out;
} TadAskerStats;

/** The session. Keep it alive while attached; the node holds a pointer to it. */
typedef struct TadAsker
{
    XmsgSession base; /**< must be first */
    TadAskerCallbacks cb;
    TadAskerConfig cfg;
    char system_name[16];
    TadAskerState state;
    int error;
    uint16_t tadadm_port;  /**< the *TADADM port, from the accept's source port */
    uint16_t session_port; /**< from the port assignment */
    uint8_t lun[2];        /**< the 0x0B data of the port assignment, as received */
    bool rfi_open;         /**< input granted and not yet used */
    /* Frames waiting to go, oldest first. */
    uint8_t queue[TAD_ASKER_QUEUE];
    uint8_t q_head;
    uint8_t q_count;
    /* The one frame sent and not yet acknowledged. */
    TadAskerSend inflight;
    uint16_t inflight_flags1;
    uint32_t inflight_since_ms;
    uint8_t xense_tries;
    uint32_t state_since_ms;
    /* Typed text waiting for, or riding in, a BDAT. */
    uint8_t line[TAD_MAX_DATA];
    uint8_t line_len;
    TadAskerStats stats;
} TadAsker;

/**
 * @brief Fill a config with the measured defaults.
 * @param cfg         Config.
 * @param peer_node   The host's node number.
 * @param system_name The host's system name as XROUT knows it (copied at connect).
 */
void tad_asker_config_default(TadAskerConfig *cfg, uint16_t peer_node, const char *system_name);

/**
 * @brief Prepare a session.
 * @param a   Session.
 * @param cfg Settings (copied).
 * @param cb  Callbacks (copied); may be NULL.
 * @return XMSG_OK, or XMSG_EINVAL for a NULL argument, a missing or over-long system name.
 */
int tad_asker_init(TadAsker *a, const TadAskerConfig *cfg, const TadAskerCallbacks *cb);

/**
 * @brief Attach to a node and send the connect letter.
 * @param a      Session, initialised and idle (or closed or failed: it starts over).
 * @param node   The node; its link must be ready.
 * @param now_ms The time.
 * @return XMSG_OK; XMSG_ESTATE when the session is in use or the link is not ready;
 *         XMSG_ENOSPC when the node has no free session place.
 */
int tad_asker_connect(TadAsker *a, XmsgNode *node, uint32_t now_ms);

/**
 * @brief Type: queue bytes as one BDAT.
 * @param a      Session.
 * @param bytes  The bytes.
 * @param len    Their count.
 * @param now_ms The time.
 * @return How many bytes were taken (1..255); XMSG_EAGAIN when the host has not granted input
 *         or the previous text is still on its way; XMSG_ESTATE when not running; XMSG_EINVAL.
 */
int tad_asker_write(TadAsker *a, const uint8_t *bytes, size_t len, uint32_t now_ms);

/** @brief Whether the host has granted input that is not yet used. */
bool tad_asker_may_type(const TadAsker *a);

/** @brief Current state. */
TadAskerState tad_asker_state(const TadAsker *a);

/**
 * @brief Drop the session here and detach from the node. Sends NOTHING (see the file comment).
 * @param a Session.
 */
void tad_asker_abort(TadAsker *a);

/** @brief Plain-words name of a state. */
const char *tad_asker_state_name(TadAskerState s);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_TAD_ASKER_H */
