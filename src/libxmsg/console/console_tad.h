/*
 * console_tad.h - a SINTRAN caller (CONNECT-TO) as a console line: a TadResponder bridged to a
 * ConsoleSession.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * A SINTRAN user types CONNECT-TO <our system>; the responder (tad/tad_responder.h) answers,
 * and once the session runs the user gets the same login and command line as a telnet user
 * (Ronny, 2026-11-10: "an allowed incoming connection ... as it came from a telnet port").
 *
 * The line sits between two sides that may live in two tasks (XMSGGW: the node task owns the
 * responder, the telnet task runs the console). Everything that crosses goes through two
 * queues in this struct, guarded by the optional lock hooks:
 *  - the NODE side (the task that owns the node): console_tad_offer() with each port-0 letter,
 *    console_tad_pump() each pass. The responder's callbacks run there too.
 *  - the CONSOLE side: the ConsoleLine from console_tad_line(), console_tad_deliver() each pass,
 *    console_tad_attach() / console_tad_release().
 * A single-threaded application (host_tad_telnet) leaves the lock hooks NULL and calls both.
 *
 * What goes to the caller, in the order the core wrote it: text (tad_responder_write), the
 * echo state (tad_responder_echo: ECKM 01 when the caller's machine is to echo, FF for a
 * password - the caller's machine echoes, so the core never does), "logged in"
 * (tad_responder_login_state(TAD_SYCN_LOGGED_IN): by the C# notes it stops D100's one-minute
 * "not logged in" timer; unverified, tad_responder.h) and the end of a burst (a frame
 * boundary). At RUNNING the line first sends ECKM 01 and SYCN 0002, as the real host starts a
 * login. The caller may type (RFI) whenever everything queued is handed to the responder and no
 * grant is out: XMSGGW tad_in.cpp's rule, used live. The core's close is a hang-up
 * (tad_responder_hangup) once the queue is empty, so "bye" goes first.
 *
 * Capabilities: the caller's machine echoes (CONSOLE_LINE_OWN_ECHO); its escape key comes as
 * ESCA (CONSOLE_LINE_ESCAPE_MSG), so an ESC byte is a function key's; and, by the config, 7-bit
 * text (CONSOLE_LINE_7BIT; on by default - a TDV in Norwegian 7-bit is the reason the rule
 * exists).
 */
#ifndef XMSG_CONSOLE_TAD_H
#define XMSG_CONSOLE_TAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libxmsg/console/console.h"
#include "libxmsg/tad/tad_responder.h"
#include "libxmsg/xmsg/xmsg_node.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Bytes queued for the caller (a WHO of 20 users is about 1.7 KB). */
#define CONSOLE_TAD_OUT_MAX 2048u
/** Typed bytes queued for the console. */
#define CONSOLE_TAD_IN_MAX 256u

/** Where the line stands. */
typedef enum ConsoleTadState
{
    CONSOLE_TAD_IDLE = 0, /**< no call; the next connect letter may come */
    CONSOLE_TAD_COMING,   /**< accepted, the bring-up is running */
    CONSOLE_TAD_RUNNING,  /**< the console may start on it */
    CONSOLE_TAD_ENDED     /**< closed or failed; console_tad_release() makes it IDLE */
} ConsoleTadState;

/** Settings; console_tad_config_default() fills them. */
typedef struct ConsoleTadConfig
{
    void *app_ctx;
    /** A call came: 0 accepts it, an XROUT error code refuses it (4, XRNSP: no free seat).
     *  Node side. NULL accepts every call. */
    int (*incoming)(void *app_ctx, const TadResponderCaller *who);
    /** The counts for a status question (LI-SYS): places in use and in all. Node side. NULL:
     *  the responder's own count. */
    void (*status)(void *app_ctx, uint16_t *in_use, uint16_t *total);
    /** Guard the queues when the two sides run in two tasks; NULL when they do not. */
    void (*lock)(void *app_ctx);
    void (*unlock)(void *app_ctx);
    bool seven_bit;               /**< CONSOLE_LINE_7BIT on the line: true */
    TadResponderConfig responder; /**< the responder's settings (give each its logical unit) */
} ConsoleTadConfig;

/** One SINTRAN caller's line. Keep it alive while a call is on it: the node holds pointers. */
typedef struct ConsoleTadLine
{
    TadResponder r;
    ConsoleTadConfig cfg;
    ConsoleTadState state;
    bool greeted;            /**< ECKM 01 and SYCN 0002 sent at RUNNING */
    bool escape;             /**< the caller pressed the escape key; not yet delivered */
    bool close_wanted;       /**< the core closed the line: hang up once the queue is out */
    uint8_t echo_sent;       /**< 0 not known, 1 on (ECKM 01), 2 off (ECKM FF) */
    ConsoleSession *session; /**< console side: the session on this line, NULL when none */
    bool had_session;        /**< a session was attached during this call */
    char who[40];            /**< "system 100 port 0x08F0" */
    uint8_t out[CONSOLE_TAD_OUT_MAX];
    size_t out_len;
    uint8_t in[CONSOLE_TAD_IN_MAX];
    size_t in_len;
    uint32_t lost_in;  /**< typed bytes dropped, queue full */
    uint32_t lost_out; /**< bytes for the caller dropped, queue full */
} ConsoleTadLine;

/**
 * @brief Defaults: no hooks, 7-bit on, tad_responder_config_default().
 * @param cfg Config.
 */
void console_tad_config_default(ConsoleTadConfig *cfg);

/**
 * @brief Prepare a line (IDLE).
 * @param t   Line.
 * @param cfg Settings (copied).
 * @return 0, or -1 for a NULL argument.
 */
int console_tad_init(ConsoleTadLine *t, const ConsoleTadConfig *cfg);

/**
 * @brief NODE side: offer a port-0 letter to this line (from the node's on_letter).
 * @param t      Line.
 * @param node   The node.
 * @param dg     The letter.
 * @param now_ms Time.
 * @return As tad_responder_offer(): 1 handled, 0 not a TAD connect letter, XMSG_EAGAIN this
 *         line is in use (offer it to the next), other negative errors.
 */
int console_tad_offer(ConsoleTadLine *t, XmsgNode *node, const XmsgDatagram *dg, uint32_t now_ms);

/**
 * @brief NODE side: move what the console queued into the responder, grant input, hang up.
 *        Call on every pass, before xmsg_node_tick().
 * @param t      Line.
 * @param now_ms Time.
 */
void console_tad_pump(ConsoleTadLine *t, uint32_t now_ms);

/**
 * @brief CONSOLE side: the line port to give console_host_open().
 * @param t   Line.
 * @param out Receives the port.
 */
void console_tad_line(ConsoleTadLine *t, ConsoleLine *out);

/**
 * @brief CONSOLE side: the session opened on this line; deliver() feeds it from now on.
 * @param t Line.
 * @param s The session (NULL to detach).
 */
void console_tad_attach(ConsoleTadLine *t, ConsoleSession *s);

/**
 * @brief CONSOLE side: whether this call is running and has not had a session yet, so the
 *        application should open one now (console_host_open + console_tad_attach). After the
 *        core closed the line the call stays RUNNING until the hang-up is done; this says false
 *        then, so no second session is opened on it.
 * @param t Line.
 * @return true when a session should be opened.
 */
bool console_tad_needs_session(const ConsoleTadLine *t);

/**
 * @brief CONSOLE side: feed what the caller typed and its escape key to the attached session;
 *        when the call has ended, tell the session the line is lost. Call on every pass.
 * @param t      Line.
 * @param now_ms Time.
 * @return The line's state after it.
 */
ConsoleTadState console_tad_deliver(ConsoleTadLine *t, uint32_t now_ms);

/**
 * @brief CONSOLE side: an ENDED line with no session becomes IDLE, ready for the next call.
 * @param t Line.
 * @return true when the line is IDLE now.
 */
bool console_tad_release(ConsoleTadLine *t);

/**
 * @brief The line's state (read without the lock: a status, at most one pass old).
 * @param t Line.
 * @return The state.
 */
ConsoleTadState console_tad_state(const ConsoleTadLine *t);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_CONSOLE_TAD_H */
