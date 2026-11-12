/*
 * tad_responder.h - a terminal session FROM a SINTRAN machine: the side that answers.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * A SINTRAN user types CONNECT-TO <our system>. Their machine (the asker) sends a letter to
 * port 0 of our node naming "*TADADM" and our system name; this responder answers it, and the
 * application then gives that user a login and a command line, as if they came in on telnet.
 *
 * The authority is a real D102 answering a real D100 (RetroIO pcap-decode-sample.json,
 * records 244-341, the host frames in extras/tests/vectors/tad_connect_d100_d102.h) and six more
 * complete host ladders in the same file (D102 answering D100 and D103, D100 answering D102,
 * D103 answering D102). test_tad_responder.c replays that session and checks every frame we
 * send byte for byte. The C# reference is RetroIO TadServer.cs (Xmsg.Servers) and
 * TadTerminalResponder.cs (Xmsg.Node); where they and the capture disagree, the capture wins
 * and the place is named below.
 *
 *   asker -> port 0          letter: "*TADADM", our system name     (0x86, 0xE4, 0x0400)
 *   us    -> asker           accept from our *TADADM port, body 00 41 00 08 01 02 00 00
 *                            02 02 00 0A                            (0x86, 0x40, 0x0400)
 *   asker -> *TADADM port    session setup 06 00 1B 00 1C 01 00 FF 00
 *   us    -> asker           port assignment from *TADADM: 07 = 00 + our system + our SESSION
 *                            port, 1F 4C 00 00, 0B = logical unit, 15 01 08, FF (0x86, 0x40)
 *   us    -> asker           DUMM from the session port             (0x92, 0x00, 0x0108)
 *   asker -> session port    terminal setup TMOD/TTYP/DESC/OPSV: no answer
 *   asker                    ESCA      -> us: ESRS (0x86, 0x00, 0x0008), RESE (0x96)
 *   asker                    RECO      -> us: RESE (0x92)
 *   asker                    RECO      -> the session is RUNNING: the application sends its
 *                                         banner (the real host: BMMX, ECKM 01, banner text,
 *                                         SYCN 0002, "ENTER ", RFI)
 *   then text both ways; the user's ESCA; the asker's DCON when its user is done
 *   us, hanging up           0xFD from *TADADM (0x82, 0x54, 0x0006), then DCON (see below)
 *
 * Every frame we send takes OUR OWN next Flags1 to that peer (the node allocates it): the real
 * D102 answers letter 00F8 with 012F and setup 00F9 with 0130. MEASURED, the two counts 0x37
 * apart. Sub-header frame flags and role bytes are copied per frame type from the capture.
 *
 * What the application does, and what the library does for it:
 *  - text OUT: tad_responder_write() appends to a BDAT. A BDAT holds at most 255 bytes; one that
 *    reaches 255 ends its frame, the way the real host sends a long listing (frames 013A-0140:
 *    each exactly one 255-byte BDAT). Bytes go untouched (8-bit clean).
 *  - the other host messages a SINTRAN login uses go in the same chain: tad_responder_echo()
 *    (ECKM: 0x01 the asker echoes typed keys, 0xFF it does not - a password, 0x00 at logout),
 *    tad_responder_login_state() (SYCN), tad_responder_set_escape() (CESC),
 *    tad_responder_put() for anything else (BMMX). The captured login ladder is in
 *    test_tad_responder.c. IMPORTANT for a live D100: the C# notes (TadTerminalResponder.cs)
 *    say SYCN 000A (logged in) is what stops D100's one-minute "not logged in" timer; that is
 *    the C# reading, not a decode - send SYCN 000A after a login, as the real host does.
 *  - input: the remote user may type only after we grant it (RFI). tad_responder_grant_input()
 *    adds the RFI; each BDAT that arrives uses the grant (text_in callback) and the application
 *    grants again when it is ready for more. MEASURED: every asker BDAT in the capture follows
 *    a host RFI. The asker sends whatever its user typed since the grant: a line, part of one,
 *    control characters (record "6C 6F F5 81"), so text_in is a byte stream, not lines.
 *  - parity: the real asker sends keys with even parity in bit 7 ("sys" CR = F3 F9 F3 8D).
 *    `strip_parity` (default on) clears bit 7 before text_in; turn it off for 8-bit data.
 *  - frames: everything added between two tad_responder_send() calls is one frame. A chain the
 *    application leaves open goes out by itself at the next free moment, so send() is only
 *    needed to force a frame boundary. One frame is in flight at a time (our choice, as in the
 *    asker); a frame refused with XENSE is sent again at the stepped-down number, 8 tries.
 *  - output pacing: the asker answers each content frame without an RFI with one DUMM (or CERS
 *    when it holds a CESC); see tad_asker.h for the measured rule. The real host waits for those
 *    answers: in the listing it never had more than 2 content frames unanswered (013A/013B, then
 *    013C/013D after the DUMM for 013B, ...). So a content frame waits while 2 are unanswered
 *    (TAD_RESP_OUTPUT_WINDOW). MEASURED in that one listing; the rule behind it is inferred.
 *  - escape: the asker's ESCA (its user pressed ESC) is answered with ESRS when escape is
 *    enabled and the application is told; with EDRS when disabled (CESC 00), and the application
 *    is not told (SINTRAN J 20-COS-TAD-POF-CODE.NPL ESCDIS, as cited in tad_asker.h). Enabled at
 *    the start: the bring-up ESCA is answered with ESRS.
 *  - the far end ends it: a DCON from the asker closes the session (state CLOSED, reason
 *    TAD_RESP_END_REMOTE). MEASURED: the real host sends nothing back (records 1260 -> only an
 *    ACK, then the next session's accept).
 *  - we end it: tad_responder_hangup(). A real host, on LOGOUT, sends "--EXIT--" with SYCN 000B,
 *    CESC 01 and then 0xFD, and never a DCON - the asker hangs up. Real D100's asker does not
 *    hang up on 0xFD by itself: by the C# notes it waits for its idle timer, about a minute. So
 *    hangup() sends the 0xFD and then a DCON of our own from the session port (0x82, role 0x00,
 *    0x0008). That DCON is NOT seen from any real host; the C# (TadServer.cs BuildDconIndication)
 *    reports it LIVE-VERIFIED against D100 on 2026-07-05: D100 prints "-- DISCONNECTED BY TAD --"
 *    at once. The logout text and SYCN 000B are the application's to send before hanging up.
 *
 * NOT verified - kept as observed constants, or flagged where used:
 *  - accept parameters 01 02 00 00 02 02 00 0A: 17 of 18 captured accepts; one says 1 / 9. The
 *    C# reads "in use / free of ten"; no decode.
 *  - port assignment 1F 03 4C 00 00 (every captured host sends it; an asker sends 4C 01 04) and
 *    15 02 01 08 (7FBSI tag, meaning of 01 08 unknown). The 0B data is a 16-bit logical unit,
 *    0x0300 + n (C#: "LU = 768 + index"); captured hosts hand out 0300..0304 from free-slot
 *    state. Ours comes from the config.
 *  - frame flags 0x92 against 0x96 (registry: "the rule for bit 0x04 is not known"). MEASURED in
 *    all 7 host ladders: 0x92 on the priming DUMM, the second RESE and the password prompt (the
 *    frame with ECKM FF), 0x96 on every other content frame. We copy that: 0x92 for a chain that
 *    holds ECKM 0xFF.
 *  - EDRS: no capture has a host sending one; it is shaped like the captured ESRS.
 *  - a refusal (tad_responder_refuse() and an `incoming` callback that refuses): no capture holds
 *    one - all 18 replies accept. XROUT reports a refusal by overwriting the service byte of the
 *    reply with its error code (xrout_letter.h), so ours is the accept's head with that byte
 *    replaced and no parameters, from port 0 (XROUT's). The asker reads only the service byte.
 *
 * The operator side (SINTRAN, not this library): the calling machine must know our system and
 * route to it, as for any XMSG traffic - on the ND that is the remote-system definition for our
 * node (DEF-REMOTE / DEFINE-FRIEND-SYSTEM in the XMSG/COSMOS set-up). Whether CONNECT-TO needs
 * anything beyond that is not established here.
 */
#ifndef XMSG_TAD_RESPONDER_H
#define XMSG_TAD_RESPONDER_H

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

/* Control words (XMCSM, also Flags2) and roles on the frames a host sends. MEASURED. */
#define TAD_RESP_XMCSM_LETTER   0x0400u /**< the accept and the port assignment */
#define TAD_RESP_XMCSM_TERMINAL 0x0108u /**< DUMM, RESE, terminal text */
#define TAD_RESP_XMCSM_BARE     0x0008u /**< ESRS, EDRS, our DCON */
#define TAD_RESP_XMCSM_NOTICE   0x0006u /**< the 0xFD notice */
#define TAD_RESP_ROLE_ADM       0x40u   /**< accept, port assignment */
#define TAD_RESP_ROLE_TERMINAL  0x00u   /**< everything from the session port */
#define TAD_RESP_ROLE_NOTICE    0x54u   /**< the 0xFD notice */

/* SYCN values a real host sends during a login (MEASURED in every host ladder; the names are
 * the C# SycnState reading). */
#define TAD_SYCN_WAIT_USER     0x0002u /**< with the "ENTER " prompt */
#define TAD_SYCN_USER_TAKEN    0x0003u /**< after the user name */
#define TAD_SYCN_PASSWORD_OK   0x0006u /**< after the password, with "OK" */
#define TAD_SYCN_LOGGED_IN     0x000Au /**< at every command prompt */
#define TAD_SYCN_LOGOUT        0x000Bu /**< with "--EXIT--": the login ended */
#define TAD_SYCN_COMMAND_ERROR 0x000Cu /**< seen before an error text (record 1329) */

/** A status question's reply goes from this port word, the *TADADM port the real D102 and
 *  D100 answer from (records 105, 381, 455). Copied: we have no fixed *TADADM port. */
#define TAD_RESP_STATUS_PORT 0x0156u

/** The "in use + free" total in a status reply: 0 + 10 in 17 of 18 captured accepts. */
#define TAD_RESP_STATUS_POOL 10u

/** The name of the terminal service on every SINTRAN machine (as TAD_SERVICE_NAME). */
#define TAD_RESP_SERVICE_NAME "*TADADM"

/** Chains (frames) waiting to go, the one being built included. */
#define TAD_RESP_CHAINS 4u

/** Bytes one chain may hold: a full BDAT (257) and the SYCN/prompt/RFI after it. */
#define TAD_RESP_CHAIN_MAX 300u

/** Content frames that may be unanswered (no DUMM/CERS yet) before the next one waits. */
#define TAD_RESP_OUTPUT_WINDOW 2u

/** XENSE resend limit. */
#define TAD_RESP_XENSE_TRIES 8u

/** Control frames that can wait at once. */
#define TAD_RESP_QUEUE 16u

typedef enum TadResponderState
{
    TAD_RESP_IDLE = 0,
    TAD_RESP_ACCEPTING, /**< accept sent, waiting for the session setup */
    TAD_RESP_BRINGUP,   /**< port assigned, running the ESCA / RECO / RECO ladder */
    TAD_RESP_RUNNING,   /**< text flows: the application sends its banner now */
    TAD_RESP_CLOSING,   /**< tad_responder_hangup(): 0xFD and DCON going out */
    TAD_RESP_CLOSED,    /**< ended (see TadResponderEnd); detached from the node */
    TAD_RESP_FAILED     /**< refused frame, timeout, no room; detached from the node */
} TadResponderState;

/** Why a session reached TAD_RESP_CLOSED (the `reason` of the state callback). */
typedef enum TadResponderEnd
{
    TAD_RESP_END_NONE = 0,
    TAD_RESP_END_REMOTE, /**< the asker sent DCON: its user is done */
    TAD_RESP_END_LOCAL,  /**< our hang-up's DCON was acknowledged */
    TAD_RESP_END_ABORT   /**< tad_responder_abort() */
} TadResponderEnd;

/** Who is calling: from the connect letter. */
typedef struct TadResponderCaller
{
    uint16_t peer_node; /**< the LAPB node the letter came over */
    uint16_t system;    /**< the asker's system number */
    uint16_t port;      /**< the asker's port word: every frame of ours goes to it */
    char service[16];   /**< string parameter 1, "*TADADM" */
    char target[24];    /**< string parameter 2: the system name the user asked for */
} TadResponderCaller;

/** What the asker said about its terminal in the terminal setup (0 until it came). */
typedef struct TadResponderTerminal
{
    bool seen;
    uint8_t tmod;    /**< TMOD, captured 08 */
    uint8_t ttyp[2]; /**< TTYP, high byte first, captured 00 00 */
    uint8_t desc;    /**< DESC: the escape key, captured 1B */
    uint8_t opsv[3]; /**< OPSV, captured 4C 01 04 */
} TadResponderTerminal;

/** What the session tells the application. Any pointer may be NULL. */
typedef struct TadResponderCallbacks
{
    void *ctx;
    /** A connect letter for this responder. Return 0 to accept, or an XROUT error code
     *  (XRNSP 4 = no free seat, ...) to refuse it. NULL accepts every call. */
    int (*incoming)(void *ctx, const TadResponderCaller *who);
    /** State changed. CLOSED: `reason` is a TadResponderEnd. FAILED: an XMSG error (negative). */
    void (*state)(void *ctx, TadResponderState state, int reason);
    /** The remote user typed: the BDAT bytes (bit 7 cleared when strip_parity). The input grant
     *  is used; call tad_responder_grant_input() when ready for more. */
    void (*text_in)(void *ctx, const uint8_t *bytes, size_t len);
    /** The remote user pressed the escape key (ESCA) while escape is enabled; ESRS is sent. */
    void (*escape)(void *ctx);
    /** A status question (LI-SYS on the caller) came: fill the TAD sessions in use and the
     *  total this application offers (XMSGGW: 8). The reply says in use and total - in use.
     *  NULL: this responder alone, 1 or 0 in use of TAD_RESP_STATUS_POOL. */
    void (*status)(void *ctx, uint16_t *in_use, uint16_t *total);
} TadResponderCallbacks;

/** Settings; tad_responder_config_default() fills them. */
typedef struct TadResponderConfig
{
    uint16_t logical_unit;     /**< the 0B data of the port assignment: 0x0300 (see the file
                                     comment); give each responder its own */
    bool strip_parity;         /**< clear bit 7 of typed bytes: true */
    uint32_t ack_timeout_ms;   /**< how long to wait for the asker's ACK of a frame: 10000 */
    uint32_t setup_timeout_ms; /**< from the letter to RUNNING: 30000 */
} TadResponderConfig;

/** Counters for tests and diagnostics. */
typedef struct TadResponderStats
{
    uint32_t letters;        /**< connect letters taken */
    uint32_t status_queries; /**< status questions answered (parameter 4 present) */
    uint32_t refused;        /**< of which refused by the application */
    uint32_t frames_in;
    uint32_t frames_out;
    uint32_t repeats_in; /**< asker frames seen again */
    uint32_t xense_resends;
    uint32_t restart_resends;
    uint32_t escapes;     /**< user ESCA reported to the application */
    uint32_t escapes_off; /**< user ESCA answered EDRS (escape disabled) */
    uint32_t answers_in;  /**< DUMM / CERS / RECO after RUNNING */
    uint32_t text_bytes_in;
    uint32_t text_bytes_out;
} TadResponderStats;

typedef struct TadResponder TadResponder;

/** One of the two ports a responder listens on; the node sees only `base`. */
typedef struct TadResponderPort
{
    XmsgSession base; /**< must be first */
    TadResponder *owner;
} TadResponderPort;

/** One waiting chain. */
typedef struct TadResponderChain
{
    uint8_t bytes[TAD_RESP_CHAIN_MAX];
    uint16_t len;
    int16_t bdat_at; /**< offset of an open BDAT that text may extend; -1 none */
} TadResponderChain;

/** The session. Keep it alive while attached; the node holds pointers into it. */
struct TadResponder
{
    TadResponderPort adm;  /**< our *TADADM port: the accept, the setup, the 0xFD */
    TadResponderPort term; /**< our session port: the terminal */
    TadResponderCallbacks cb;
    TadResponderConfig cfg;
    TadResponderState state;
    uint32_t state_since_ms;
    TadResponderCaller caller;
    TadResponderTerminal terminal;
    uint8_t reco_count;   /**< RECOs in the bring-up */
    bool escape_enabled;  /**< from the last CESC we added */
    bool input_granted;   /**< an RFI has gone into a chain and no BDAT has used it */
    uint8_t answers_owed; /**< content frames the asker has not answered yet */
    /* Control frames waiting, oldest first (TadResponderSend values); chains are not in it. */
    uint8_t queue[TAD_RESP_QUEUE];
    uint8_t q_head;
    uint8_t q_count;
    /* Chains: [c_head, c_head + c_closed) are closed and queued; the next one is being built. */
    TadResponderChain chains[TAD_RESP_CHAINS];
    uint8_t c_head;
    uint8_t c_closed;
    /* The one frame in flight. */
    uint8_t inflight; /**< a TadResponderSend value; 0 none */
    uint16_t inflight_flags1;
    uint32_t inflight_since_ms;
    uint8_t xense_tries;
    TadResponderStats stats;
};

/** @brief Fill a config with the defaults named in the struct. */
void tad_responder_config_default(TadResponderConfig *cfg);

/**
 * @brief Prepare a responder (idle, not attached).
 * @return XMSG_OK or XMSG_EINVAL for a NULL responder or config.
 */
int tad_responder_init(TadResponder *r, const TadResponderConfig *cfg,
                       const TadResponderCallbacks *cb);

/**
 * @brief Offer a port-0 letter (from the node's on_letter callback) to this responder.
 *
 * A TAD connect letter is an XSLET letter whose string parameter 1 is "*TADADM". For one, the
 * `incoming` callback decides; accepted, the responder attaches its two ports to the node and
 * queues the accept. A repeat of a letter (dg->repeat) is taken and ignored: the node has
 * ACKed it again and the first copy was answered.
 *
 * @param r      Responder.
 * @param node   The node the letter came in on.
 * @param dg     The letter.
 * @param now_ms Time.
 * A status question (tad_responder_is_status_query) is answered here and opens nothing.
 *
 * @return 1 when the letter was a TAD connect and was handled (accepted, refused, a repeat, or a
 *         status question answered);
 *         0 when it is not a TAD connect letter (offer it elsewhere); XMSG_EAGAIN when this
 *         responder is in use (offer it to the next one, or tad_responder_refuse());
 *         XMSG_ENOSPC when the node has no two free session places (the call was refused with
 *         XRNSP); XMSG_EINVAL.
 */
int tad_responder_offer(TadResponder *r, XmsgNode *node, const XmsgDatagram *dg, uint32_t now_ms);

/**
 * @brief Whether a port-0 datagram is a TAD connect letter (XSLET naming "*TADADM").
 */
bool tad_responder_is_connect(const XmsgDatagram *dg);

/**
 * @brief Whether a port-0 datagram is a *TADADM STATUS question rather than a call: the letter
 *        carries integer parameter 4, of any value (`04 02 00 01` or `04 02 00 00` after the
 *        system name).
 *
 * MEASURED, live D100 2026-11-10 19:09:27 (XMSGGW): plain LI-SYS at D100's own @ prompt sends
 * the letter with parameter 4 = 0. Answered with the fixed call accept, D100 printed its two
 * integers as "in use / free" and sent no session setup - the same as for 1. What 0 versus 1
 * means is not known; no capture of a real machine answering 0 exists here.
 *
 * MEASURED: D102 and D103 send such letters to D100 (pcap-decode-sample.json records 218, 378,
 * 446) and D100 answers each with the plain accept from its *TADADM port, after which the asker
 * sends NOTHING more: no session setup. Live D100 sends one to us for every system when a user
 * runs LI-SYS inside CONNECT-TO (2026-11-10): no answer hangs LI-SYS on our row. LI-SYS prints
 * "T-ACC running" and "TADs in use / free" for each system, which fits the accept's two
 * integers (in use, free) - an inference from the printout, not a decode. What other values of
 * parameter 4 mean is not known; a call letter (ours, and the real asker's in this capture)
 * has no parameter 4.
 */
bool tad_responder_is_status_query(const XmsgDatagram *dg);

/**
 * @brief Answer a status question: the accept with `in_use` and `free_count`, from
 *        TAD_RESP_STATUS_PORT. tad_responder_offer() does this by itself, with the counts from
 *        the `status` callback (or its own state when that is NULL).
 * @return XMSG_OK; XMSG_EAGAIN (window full); XMSG_ESTATE; XMSG_EINVAL.
 */
int tad_responder_status_reply(XmsgNode *node, const XmsgDatagram *dg, uint16_t in_use,
                               uint16_t free_count, uint32_t now_ms);

/**
 * @brief Refuse a connect letter no responder can take: the reply with the XROUT error code in
 *        its service byte, from port 0. UNVERIFIED shape, see the file comment.
 * @param node   Node.
 * @param dg     The letter.
 * @param code   XROUT error code, e.g. 4 (XRNSP, no free seat).
 * @param now_ms Time.
 * @return XMSG_OK; XMSG_EAGAIN when the window has no room (the asker sends the letter again);
 *         XMSG_EINVAL; XMSG_ESTATE when the link is not ready.
 */
int tad_responder_refuse(XmsgNode *node, const XmsgDatagram *dg, uint8_t code, uint32_t now_ms);

/**
 * @brief Text to the remote user: appended to the open BDAT, a new one when there is none.
 *        A BDAT that reaches 255 bytes ends its frame and the text goes on in the next.
 * @return Bytes taken (all, unless every chain is full: then fewer); XMSG_EAGAIN when none could
 *         be taken; XMSG_ESTATE when the session is not accepted or is ending; XMSG_EINVAL.
 */
int tad_responder_write(TadResponder *r, const uint8_t *bytes, size_t len);

/**
 * @brief Any other TAD message into the open chain (BMMX, ...), with the host's alignment
 *        (tad_writer_add_host). CESC also sets the escape state; RFI also grants input.
 * @return XMSG_OK; XMSG_EAGAIN when every chain is full; XMSG_ESTATE; XMSG_EINVAL.
 */
int tad_responder_put(TadResponder *r, uint8_t opcode, const uint8_t *data, size_t len);

/** @brief ECKM: 0x01 the asker echoes typed keys, 0xFF it does not, 0x00 at logout. */
int tad_responder_echo(TadResponder *r, uint8_t eckm);

/** @brief SYCN with a 16-bit value (TAD_SYCN_*). */
int tad_responder_login_state(TadResponder *r, uint16_t sycn);

/** @brief CESC: whether the user's escape key breaks (ESRS) or is refused (EDRS). */
int tad_responder_set_escape(TadResponder *r, bool enabled);

/** @brief RFI: the remote user may type (once: until their next BDAT arrives). */
int tad_responder_grant_input(TadResponder *r);

/**
 * @brief End the frame being built: what was added since the last call goes out as one frame.
 * @return XMSG_OK (also when the chain is empty: nothing happens); XMSG_EAGAIN when no chain is
 *         free for the next one (the closed one waits); XMSG_ESTATE; XMSG_EINVAL.
 */
int tad_responder_send(TadResponder *r);

/** @brief Whether an RFI is out and the remote user has not used it yet. */
bool tad_responder_input_granted(const TadResponder *r);

/**
 * @brief Hang up: what was added is sent, then the 0xFD notice and our DCON. CLOSED (reason
 *        TAD_RESP_END_LOCAL) once the asker acknowledges the DCON, or (reason
 *        TAD_RESP_END_REMOTE) at once if its own DCON comes first. Before RUNNING the text
 *        waiting is dropped.
 * @return XMSG_OK; XMSG_ESTATE when there is no session; XMSG_EINVAL.
 */
int tad_responder_hangup(TadResponder *r, uint32_t now_ms);

/** @brief Drop the session here and detach; sends nothing (the line is gone). */
void tad_responder_abort(TadResponder *r);

/** @brief Current state. */
TadResponderState tad_responder_state(const TadResponder *r);

/** @brief Plain-words name of a state. */
const char *tad_responder_state_name(TadResponderState s);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_TAD_RESPONDER_H */
