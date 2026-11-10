/*
 * console.h - the bridge's login and command line, the same for every way a user arrives.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * One piece of code for telnet, SSH, an incoming CONNECT-TO from a SINTRAN machine, a USB or
 * serial console: the login ("login: ", "password: ", the forced new password), the prompt
 * (TAD> in the host tool, XMSGGW> on the ESP32) and its commands - CONNECT, LIST, WHO, INFO,
 * PASSWORD, HELP, EXIT/LOGOUT/QUIT and for admins USERS, ADDUSER [ADMIN], DELUSER, RESETPW - and
 * the bridge into an onward terminal session: typed bytes on, host text back, the escape key,
 * and the SINTRAN logout that ends it. It was in examples/host_tad_telnet/host_tad_telnet.c
 * and, as a copy, in XMSGGW telnet_task.cpp; both now run on this.
 *
 * Two parts:
 *  - ConsoleSession: one user. A state machine: login, prompt, a password being typed, an
 *    onward session, or gone. ConsoleHost: what the sessions share - the table WHO reads, the
 *    in-use and free counts a SINTRAN status question gets, the user store and the mode
 *    (direct: straight into a session on one system; prompt: login first).
 *  - two ports the application implements. ConsoleLine is the ONLY way the core reaches its
 *    user (write bytes, end of output, echo, close, describe). ConsoleServices is the ONLY way
 *    it reaches the XMSG node (open, type into, escape and hang up an onward session; start and
 *    stop a LIST walk; the link facts for INFO). Results come back through the console_onward_*
 *    and console_list_* feed calls.
 *
 * Rules the application keeps:
 *  - The core owns no socket and never calls the node. It is never called from two threads at
 *    once: one thread (one FreeRTOS task) runs every console_* call on a host.
 *  - Never call a console_* function from inside a ConsoleLine or ConsoleServices callback.
 *    Record what happened and feed it after the callback returns (the host tool does it in its
 *    main loop; XMSGGW does it when the node task's answer reaches the telnet task).
 *  - Memory is fixed: no allocation. The caller supplies the session array (XMSGGW can put it
 *    in PSRAM). A session's place is its index in that array, fixed for its life; the onward
 *    session's terminal number is place + 1 (what TADA on the host shows).
 *  - After the core calls the line's `close`, or after the application called
 *    console_line_lost(), the session pointer is no longer the application's: the place is
 *    free again once no onward session is left on it.
 *  - After `onward_hangup` the application must report the end with console_onward_state()
 *    (CLOSED or FAILED), also when it drops the session locally. A place whose user is gone
 *    stays taken until that report comes, so its onward session cannot be reused too early.
 *
 * Text rules (Ronny, XMSGGW 2026-11-10):
 *  - On a line that says CONSOLE_LINE_7BIT, the core's own text never holds [ ] { | } \ : a
 *    Norwegian 7-bit TDV shows them as letters ("[session" came out as "AEsession"). They are
 *    sent as < > < ! > / instead (console_7bit). Text from the onward host and the user's own
 *    echo are NOT changed: those bytes are the host's and the user's letters.
 *  - "error N" is printed only when N != 0.
 *
 * Writing a line adapter (SSH, USB, serial; telnet and TAD are in the library as
 * term/term_telnet.h and console/console_tad.h): fill a ConsoleLine. `write` takes bytes for
 * the user (it may take fewer: the rest is dropped, as a client that does not read loses
 * text); `flush` marks the end of a burst of output; `echo` is only needed on a line whose far
 * side echoes (CONSOLE_LINE_OWN_ECHO); `close` must end the connection once the bytes written
 * so far are out; `describe` gives the peer for WHO. Give each byte the user types to
 * console_input(), the escape key of a line that sends it as a message to console_escape(),
 * and the loss of the line to console_line_lost(). A USB console where the admin is trusted
 * sets CONSOLE_LINE_TRUSTED_ADMIN: no login, user "admin" at level admin.
 */
#ifndef XMSG_CONSOLE_H
#define XMSG_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libxmsg/auth/auth_login.h"
#include "libxmsg/auth/auth_user.h"
#include "libxmsg/term/term_escape.h"
#include "libxmsg/term/term_keyseq.h"
#include "libxmsg/term/term_line.h"
#include "libxmsg/term/term_report.h"
#include "libxmsg/xrout/xrout_name.h"
#include "libxmsg/xrout/xrout_route.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Typed bytes held for the onward session until it takes them. */
#define CONSOLE_TYPED_MAX 256u
/** Longest system name for CONNECT (SINTRAN names; tad_asker keeps 15). */
#define CONSOLE_SYSTEM_MAX 15u
/** Room for "kind peer" in WHO. */
#define CONSOLE_REMOTE_MAX 47u
/** Longest onward TAD user name, without its zero (TAD_IAM_USER_SIZE - 1). */
#define CONSOLE_TAD_USER_MAX 16u

/* What a line can do; ConsoleLine.caps. */
#define CONSOLE_LINE_OWN_ECHO      0x01u /**< the far side echoes typed keys (a SINTRAN caller) */
#define CONSOLE_LINE_ESCAPE_MSG    0x02u /**< the escape key comes as a message (TAD ESCA), so
                                              an ESC byte is always text */
#define CONSOLE_LINE_7BIT          0x04u /**< 7-bit national text: see the text rules above */
#define CONSOLE_LINE_TRUSTED_ADMIN 0x08u /**< logged in as admin with no password (USB) */

/** Why the core closes a line. */
typedef enum ConsoleClose
{
    CONSOLE_CLOSE_EXIT = 1,      /**< the user typed EXIT, LOGOUT or QUIT */
    CONSOLE_CLOSE_LOGIN_FAILED,  /**< the tries are used up, or a needed change is impossible */
    CONSOLE_CLOSE_SESSION_ENDED, /**< direct mode: the onward session ended */
    CONSOLE_CLOSE_NO_USERS       /**< the user list cannot be used */
} ConsoleClose;

/** The way to one user. Copied by console_host_open(); `ctx` must stay valid. */
typedef struct ConsoleLine
{
    void *ctx;
    uint32_t caps;    /**< CONSOLE_LINE_* */
    const char *kind; /**< "telnet", "TAD", "SSH", "USB": shown by WHO; NULL = "line" */
    /** Bytes for the user. Returns the count taken (fewer: the rest is dropped). */
    int (*write)(void *ctx, const uint8_t *bytes, size_t len);
    /** End of a burst of output: the user may type now. May be NULL. */
    void (*flush)(void *ctx);
    /** Whether typed keys should be shown now (false while a password is typed). Needed only
     *  with CONSOLE_LINE_OWN_ECHO: the line passes it on to the far side. May be NULL. */
    void (*echo)(void *ctx, bool on);
    /** A user has logged in (TAD: SYCN 000A). May be NULL. */
    void (*logged_in)(void *ctx, const char *user);
    /** End the connection once what was written is out. The session is gone after this. */
    void (*close)(void *ctx, ConsoleClose why);
    /** The peer, for WHO ("127.0.0.1:50211", "system 100 port 0x08F0"). May be NULL. */
    void (*describe)(void *ctx, char *buf, size_t cap);
} ConsoleLine;

/** Where an onward session stands; reported with console_onward_state(). */
typedef enum ConsoleOnward
{
    CONSOLE_ONWARD_NONE = 0,
    CONSOLE_ONWARD_OPENING, /**< asked for, not running yet */
    CONSOLE_ONWARD_RUNNING,
    CONSOLE_ONWARD_CLOSED,
    CONSOLE_ONWARD_FAILED
} ConsoleOnward;

/** What the core needs from the XMSG node, by place. Every call may be marshalled to the task
 *  that owns the node; every answer comes back through a feed call. */
typedef struct ConsoleServices
{
    void *ctx;
    /** A CONNECT argument that is a plain decimal number (1..65535) is used as the system
     *  number as it is, without this call; any other text is a name. */
    /** The number of a system name, from the application's own table (as SINTRAN's
     *  DEF-REMOTE): 0 and `number` filled, or -1 for an unknown name, which CONNECT refuses.
     *  The number is never worked out from the name text. Required. */
    int (*resolve)(void *ctx, const char *system, uint16_t *number);
    /** Start an onward TAD session to `system`, addressed to the TARGET's own number `number`
     *  (from resolve; tad_asker peer_node), not the link neighbour; `tad_user` is the name TADA
     *  shows (NULL: the library default). Returns 0 when it is under way (RUNNING, CLOSED or
     *  FAILED follows), or a negative XMSG error when it cannot start at all. */
    int (*onward_open)(void *ctx, int place, const char *system, uint16_t number,
                       const char *tad_user);
    /** Typed bytes for the onward session: ONE call is at most ONE TAD data letter (tad_asker_write
     *  when the host has granted input). Returns the count taken (0 when it cannot take any
     *  now; the core offers the rest again later). A function key's escape sequence is
     *  offered alone, in its own call. How many bytes one letter carries is the TAD asker's
     *  decision under the host's break setting (tad_asker.h): in S3-CONFIG the host breaks on
     *  every character and a key goes one byte per letter, as the real SINTRAN asker sends it
     *  (measured 2026-11-10). Taking part of a call is therefore normal. Do not gather the
     *  bytes of several calls into one buffer that is cut into letters later. */
    int (*onward_write)(void *ctx, int place, const uint8_t *bytes, size_t len);
    /** The user pressed the escape key (TAD ESCA). */
    void (*onward_escape)(void *ctx, int place);
    /** End the onward session (DCON). The end is reported with console_onward_state(). */
    void (*onward_hangup)(void *ctx, int place);
    /** Start reading the host's routing table. Returns 0 (console_list_entry() and
     *  console_list_done() follow), XMSG_ESTATE (link not ready), XMSG_ENOSPC (no session
     *  slot), XMSG_EAGAIN (another LIST is running) or another negative error. NULL: no LIST. */
    int (*list_start)(void *ctx, int place);
    /** Stop the walk of a user who went away. No console_list_done() is needed after it. */
    void (*list_cancel)(void *ctx, int place);
    /** Start reading the host's name table (XSGNI, xrout_names_walk.h). Returns 0
     *  (console_names_row() and console_names_done() follow) or the same errors as list_start.
     *  NULL: no NAMES. */
    int (*names_start)(void *ctx, int place);
    /** Stop the NAMES walk of a user who went away. No console_names_done() is needed after it. */
    void (*names_cancel)(void *ctx, int place);
    /** The link facts for INFO. NULL: INFO says they are not available. */
    void (*link_info)(void *ctx, TermLinkInfo *out);
    /** More INFO lines of the application's own (CR LF ended), after the link facts. May be
     *  NULL. Returns the length written. */
    int (*info_more)(void *ctx, char *buf, size_t cap);
    /** The application's own commands. Called for a prompt line the core does not know, before
     *  "unknown command", with the line as typed and the user's level. Returns 1 when the line
     *  was the application's (`out` holds the text to print, zero-terminated, each line ended
     *  by CR LF; the core applies the 7-bit rule and shows the prompt after it), or 0 when it
     *  is not (the core says "unknown command"). The application checks the level itself.
     *  NULL: no application commands. */
    int (*command)(void *ctx, int place, const char *line, AuthLevel level, char *out,
                   size_t cap);
    /** HELP lines for the application's commands, shown to every user (CR LF ended). NULL:
     *  none. */
    const char *help_user;
    /** HELP lines shown to admins only, before the core's admin lines. NULL: none. */
    const char *help_admin;
} ConsoleServices;

/** The whole user list, for USERS and DELUSER. Looking one up and saving one go through the
 *  AuthUserStore. */
typedef struct ConsoleUsers
{
    void *ctx;
    /** The user at `index` (0, 1, ...): 0 and `out` filled, -1 past the end. NULL: no USERS. */
    int (*get)(void *ctx, size_t index, AuthUser *out);
    /** Delete a user by name. 0 when done. NULL: no DELUSER. */
    int (*remove)(void *ctx, const char *name);
} ConsoleUsers;

/** Direct: every user goes straight into a session on one system. Prompt: login first. */
typedef enum ConsoleMode
{
    CONSOLE_MODE_PROMPT = 0,
    CONSOLE_MODE_DIRECT
} ConsoleMode;

/** Settings; console_config_default() fills them. Strings are not copied: keep them alive. */
typedef struct ConsoleConfig
{
    ConsoleMode mode;
    const char *prompt;          /**< "TAD> " */
    const char *banner;          /**< shown before the login, CR LF ended; NULL = none */
    const char *direct_system;   /**< direct mode: the system every session goes to */
    const char *tad_user;        /**< onward TAD user name when there is no suffix rule (and
                                      in direct mode); NULL = the library default */
    const char *tad_user_suffix; /**< set: "<user>@<suffix>" when that fits in 16 characters,
                                      else the user name alone (XMSGGW: "xmsggw"); NULL = off */
    uint32_t iterations;         /**< PBKDF2 count for new and re-stored records: 100000 */
    uint32_t escape_ms;          /**< lone-ESC wait (term_keyseq.h): 50; 0 = ESC is text */
    uint32_t keyseq_ms;          /**< wait for the next byte of a function key: 200 */
    uint32_t login_tries;        /**< AUTH_LOGIN_TRIES */
    uint32_t login_delay_ms;     /**< AUTH_LOGIN_DELAY_MS */
    AuthUserStore store;         /**< lookup and save */
    AuthRandom rng;              /**< salts; fill NULL = no password can be set */
    ConsoleUsers users;
    ConsoleServices services;
} ConsoleConfig;

/** Where a user is. */
typedef enum ConsolePhase
{
    CONSOLE_PH_FREE = 0, /**< the place is free */
    CONSOLE_PH_LOGIN,    /**< name and password */
    CONSOLE_PH_PROMPT,   /**< at the prompt */
    CONSOLE_PH_PASSWD,   /**< PASSWORD: changing one's own password */
    CONSOLE_PH_SETPW,    /**< ADDUSER / RESETPW: an admin types a password for another user */
    CONSOLE_PH_SESSION,  /**< in an onward session */
    CONSOLE_PH_DRAIN     /**< the user is gone; waiting for the onward session to end */
} ConsolePhase;

typedef struct ConsoleHost ConsoleHost;

/** One user. The caller owns the storage (an array given to console_host_init()). */
typedef struct ConsoleSession
{
    ConsoleHost *host;
    int place;
    ConsolePhase phase;
    ConsoleLine line;
    TermLine edit;   /**< the command line / login line editor */
    AuthLogin login; /**< prompt mode */
    TermKeyseq keys; /**< function keys whole, and the lone ESC on a line without ESCAPE_MSG */
    char user[AUTH_NAME_MAX + 1u];
    AuthLevel level;
    bool logged_in;
    AuthUser setpw;   /**< SETPW: the record being made (hash only) */
    bool setpw_again; /**< SETPW: the first entry is in, the second is due */
    bool setpw_add;   /**< SETPW: a new user (ADDUSER), not RESETPW */
    char system[CONSOLE_SYSTEM_MAX + 1u];
    char remote[CONSOLE_REMOTE_MAX + 1u];
    uint32_t connected_ms;
    ConsoleOnward onward;
    bool onward_echo; /**< the onward host wants typed keys shown (its ECKM) */
    bool hangup_sent;
    bool listing;        /**< a LIST or NAMES walk is running; the prompt waits for its end */
    bool listing_names;  /**< ... and it is NAMES */
    bool far_echo_known; /**< OWN_ECHO line: the echo state last sent */
    bool far_echo;
    bool dirty; /**< output written since the last flush */
    uint8_t typed[CONSOLE_TYPED_MAX];
    uint8_t typed_key[CONSOLE_TYPED_MAX]; /**< at a key's first byte: its length; else 0 */
    size_t typed_len;
} ConsoleSession;

/** What the sessions share. */
struct ConsoleHost
{
    ConsoleConfig cfg;
    ConsoleSession *sessions;
    size_t count;
};

/**
 * @brief Fill a config with the defaults named in the struct: prompt mode, prompt "TAD> ",
 *        no ports.
 * @param cfg Config.
 */
void console_config_default(ConsoleConfig *cfg);

/**
 * @brief Start a host over caller-owned sessions (all made free).
 * @param h        Host.
 * @param cfg      Settings (copied).
 * @param sessions The array; place i is sessions[i].
 * @param count    Its length, at least 1.
 * @return 0, or -1 for a bad argument (a NULL port function the core cannot do without:
 *         services.resolve, onward_open, onward_write, onward_hangup; store.lookup in prompt
 *         mode).
 */
int console_host_init(ConsoleHost *h, const ConsoleConfig *cfg, ConsoleSession *sessions,
                      size_t count);

/**
 * @brief A new user on a line: take the first free place and start (banner, login prompt, or
 *        in direct mode the onward session).
 * @param h      Host.
 * @param line   The line (copied).
 * @param now_ms Time.
 * @return The session, or NULL when every place is taken (tell the user and drop the line).
 */
ConsoleSession *console_host_open(ConsoleHost *h, const ConsoleLine *line, uint32_t now_ms);

/**
 * @brief As console_host_open(), at a given place (an application that hands out places
 *        itself, as XMSGGW does for SINTRAN callers).
 * @param h      Host.
 * @param place  0..count-1.
 * @param line   The line (copied).
 * @param now_ms Time.
 * @return The session, or NULL when that place is taken or out of range.
 */
ConsoleSession *console_host_open_at(ConsoleHost *h, int place, const ConsoleLine *line,
                                     uint32_t now_ms);

/**
 * @brief The session at a place, whatever its phase.
 * @param h     Host.
 * @param place 0..count-1.
 * @return The session, or NULL when out of range.
 */
ConsoleSession *console_host_session(ConsoleHost *h, int place);

/**
 * @brief Whether a place is free.
 * @param h     Host.
 * @param place 0..count-1.
 * @return true when free; false when taken or out of range.
 */
bool console_host_place_is_free(const ConsoleHost *h, int place);

/**
 * @brief Places in use and in all, for a SINTRAN status question (LI-SYS).
 * @param h      Host.
 * @param in_use Receives the places taken.
 * @param total  Receives the count of places.
 */
void console_host_counts(const ConsoleHost *h, uint16_t *in_use, uint16_t *total);

/**
 * @brief Let time pass for every session: the failed-login wait, a lone ESC, typed bytes the
 *        onward session did not take yet. Call it every loop turn.
 * @param h      Host.
 * @param now_ms Time.
 */
void console_host_poll(ConsoleHost *h, uint32_t now_ms);

/**
 * @brief Bytes the user typed (telnet commands already taken out).
 * @param s      Session.
 * @param bytes  The bytes.
 * @param len    Their count.
 * @param now_ms Time.
 */
void console_input(ConsoleSession *s, const uint8_t *bytes, size_t len, uint32_t now_ms);

/**
 * @brief The user pressed the escape key, on a line that sends it as a message (TAD ESCA).
 *        In an onward session it goes on; elsewhere it is ignored.
 * @param s Session.
 */
void console_escape(ConsoleSession *s);

/**
 * @brief The line is gone (TCP closed, the SINTRAN caller hung up). Any onward session is hung
 *        up; the place is free once it has ended. Do not use `s` afterwards.
 * @param s Session.
 */
void console_line_lost(ConsoleSession *s);

/**
 * @brief Text from the onward host, passed to the user untouched.
 * @param s     Session.
 * @param bytes The bytes.
 * @param len   Their count.
 */
void console_onward_text(ConsoleSession *s, const uint8_t *bytes, size_t len);

/**
 * @brief The onward session changed state.
 *
 * RUNNING: typing goes through. CLOSED / FAILED: "<session closed>" (or "<session failed,
 * error N>" when N != 0); then in direct mode the line is closed, in prompt mode the user is
 * back at the prompt, and a user who is already gone frees the place.
 *
 * @param s     Session.
 * @param st    CONSOLE_ONWARD_RUNNING, _CLOSED or _FAILED.
 * @param error The session's error on FAILED (0 when none).
 */
void console_onward_state(ConsoleSession *s, ConsoleOnward st, int error);

/**
 * @brief The onward host's echo wish (ECKM 01: show typed keys; FF: do not, a password).
 * @param s  Session.
 * @param on true to show typed keys.
 */
void console_onward_echo(ConsoleSession *s, bool on);

/**
 * @brief The onward host says its login ended (SYCN 000B after LOGOUT): the core hangs up.
 * @param s Session.
 */
void console_onward_logout(ConsoleSession *s);

/**
 * @brief One entry of the host's routing table, during LIST.
 * @param s Session.
 * @param e The entry.
 */
void console_list_entry(ConsoleSession *s, const XroutRouteEntry *e);

/**
 * @brief The LIST walk ended.
 * @param s       Session.
 * @param result  XMSG_OK (the host said "no more"), XMSG_ETIMEDOUT, or another error.
 * @param entries Entries reported.
 */
void console_list_done(ConsoleSession *s, int result, unsigned entries);

/**
 * @brief One row of the host's name table, during NAMES.
 * @param s   Session.
 * @param row The row.
 */
void console_names_row(ConsoleSession *s, const XroutNameRow *row);

/**
 * @brief The NAMES walk ended.
 * @param s      Session.
 * @param result XMSG_OK (the host said "no more"), XMSG_ETIMEDOUT, or another error.
 * @param rows   Rows reported.
 */
void console_names_done(ConsoleSession *s, int result, unsigned rows);

/**
 * @brief The 7-bit text rule for one byte of the core's own text.
 * @param b The byte.
 * @return '<' for '[' and '{', '>' for ']' and '}', '!' for '|', '/' for '\', else b.
 */
uint8_t console_7bit(uint8_t b);

/**
 * @brief Plain-words name of an onward state, for WHO.
 * @param st The state.
 * @return "none", "opening", "running", "closed", "failed".
 */
const char *console_onward_name(ConsoleOnward st);

/**
 * @brief The onward TAD user name for a logged-in user under the config's rule.
 * @param cfg  Config.
 * @param user The bridge user; NULL or "" when nobody is logged in.
 * @param out  Receives the name.
 * @param cap  Its size (CONSOLE_TAD_USER_MAX + 1 is enough).
 * @return out, or NULL when the library default is to be used.
 */
const char *console_tad_user_name(const ConsoleConfig *cfg, const char *user, char *out,
                                  size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_CONSOLE_H */
