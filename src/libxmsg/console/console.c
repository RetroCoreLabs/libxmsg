/*
 * console.c - the bridge's login and command line, the same for every way a user arrives.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * The steps and texts are the ones of examples/host_tad_telnet/host_tad_telnet.c (prompt and
 * direct mode, as Ronny tested them) and XMSGGW telnet_task.cpp (the SINTRAN caller, the
 * re-store at the configured PBKDF2 count, the echo rules in a session, the < > text). Where the
 * two differed, the choice is named at the place.
 */
#include "libxmsg/console/console.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "libxmsg/log/xmsg_log.h"
#include "libxmsg/xmsg_errors.h"

#define BS  0x08u
#define DEL 0x7Fu
#define CR  0x0Du

/* ------------------------------------------------------------------------------------------ */
/* Output                                                                                     */
/* ------------------------------------------------------------------------------------------ */

static bool plain_number(const char *text, uint16_t *number);

/* Copy a string, cut to fit. Returns false when it was cut. */
static bool copy_text(char *out, size_t cap, const char *src)
{
    size_t n = strlen(src);
    bool whole = n < cap;
    if (cap == 0u)
    {
        return false;
    }
    if (!whole)
    {
        n = cap - 1u;
    }
    memcpy(out, src, n);
    out[n] = '\0';
    return whole;
}

static bool line_is(const ConsoleSession *s, uint32_t cap)
{
    return (s->line.caps & cap) != 0u;
}

uint8_t console_7bit(uint8_t b)
{
    switch (b)
    {
    case '[':
        /* fall through */
    case '{':
        return (uint8_t)'<';
    case ']':
        /* fall through */
    case '}':
        return (uint8_t)'>';
    case '|':
        return (uint8_t)'!';
    case '\\':
        return (uint8_t)'/';
    default:
        return b;
    }
}

/* Bytes to the user as they are (echo, onward host text). */
static void out_raw(ConsoleSession *s, const uint8_t *b, size_t n)
{
    if (n == 0u || s->line.write == NULL)
    {
        return;
    }
    (void)s->line.write(s->line.ctx, b, n); /* fewer taken: dropped (see ConsoleLine.write) */
    s->dirty = true;
}

/* The core's own text: through the 7-bit rule when the line asks for it. */
static void say(ConsoleSession *s, const char *text)
{
    size_t n = strlen(text);
    if (!line_is(s, CONSOLE_LINE_7BIT))
    {
        out_raw(s, (const uint8_t *)text, n);
        return;
    }
    while (n > 0u)
    {
        uint8_t chunk[64];
        size_t k = (n < sizeof chunk) ? n : sizeof chunk;
        for (size_t i = 0; i < k; i++)
        {
            chunk[i] = console_7bit((uint8_t)text[i]);
        }
        out_raw(s, chunk, k);
        text += k;
        n -= k;
    }
}

static void sayf(ConsoleSession *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void sayf(ConsoleSession *s, const char *fmt, ...)
{
    char text[256];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (n > 0)
    {
        say(s, text); /* a cut line is still zero-terminated */
    }
}

/* End of a burst: tell the line, once, when something was written. */
static void flush(ConsoleSession *s)
{
    if (s->dirty && s->phase != CONSOLE_PH_FREE && s->phase != CONSOLE_PH_DRAIN)
    {
        s->dirty = false;
        if (s->line.flush != NULL)
        {
            s->line.flush(s->line.ctx);
        }
    }
}

/* Whether typed keys are shown. A line whose far side echoes is told (only on a change); the
 * line editor then never echoes. Elsewhere the line editor echoes. */
static void set_echo(ConsoleSession *s, bool on)
{
    if (line_is(s, CONSOLE_LINE_OWN_ECHO))
    {
        s->edit.echo = false;
        if (!s->far_echo_known || s->far_echo != on)
        {
            s->far_echo_known = true;
            s->far_echo = on;
            if (s->line.echo != NULL)
            {
                s->line.echo(s->line.ctx, on);
            }
        }
        return;
    }
    s->edit.echo = on;
}

/* ------------------------------------------------------------------------------------------ */
/* Session life                                                                               */
/* ------------------------------------------------------------------------------------------ */

static void wipe(ConsoleSession *s)
{
    ConsoleHost *h = s->host;
    int place = s->place;
    auth_wipe(s, sizeof *s);
    s->host = h;
    s->place = place;
    s->phase = CONSOLE_PH_FREE;
}

/* Stop a running LIST or NAMES walk of a user who went away. */
static void walk_cancel(ConsoleSession *s)
{
    const ConsoleServices *sv = &s->host->cfg.services;
    if (s->listing && s->listing_names && sv->names_cancel != NULL)
    {
        sv->names_cancel(sv->ctx, s->place);
    }
    else if (s->listing && !s->listing_names && sv->list_cancel != NULL)
    {
        sv->list_cancel(sv->ctx, s->place);
    }
    s->listing = false;
    s->listing_names = false;
}

/* The core ends the line. The session is free at once: a close never comes with an onward
 * session still open (EXIT is typed at the prompt; direct mode closes after the end). */
static void close_line(ConsoleSession *s, ConsoleClose why)
{
    ConsoleLine line = s->line;
    XMSG_LOG(XMSG_LOG_CONSOLE, XMSG_LOG_INFO, "place %d: closing the line (%d)", s->place,
             (int)why);
    walk_cancel(s);
    if (s->dirty && line.flush != NULL)
    {
        line.flush(line.ctx);
    }
    wipe(s);
    if (line.close != NULL)
    {
        line.close(line.ctx, why);
    }
}

static void show_prompt(ConsoleSession *s)
{
    const char *p;
    switch (s->phase)
    {
    case CONSOLE_PH_LOGIN:
        p = auth_login_prompt(&s->login);
        if (p != NULL)
        {
            set_echo(s, auth_login_echo(&s->login));
            say(s, p);
        }
        break;
    case CONSOLE_PH_PASSWD:
        set_echo(s, false);
        p = auth_login_prompt(&s->login);
        if (p != NULL)
        {
            say(s, p);
        }
        break;
    case CONSOLE_PH_SETPW:
        set_echo(s, false);
        sayf(s, "%s for %s: ", s->setpw_again ? "again" : "new password", s->setpw.name);
        break;
    case CONSOLE_PH_PROMPT:
        set_echo(s, true);
        say(s, (s->host->cfg.prompt != NULL) ? s->host->cfg.prompt : "> ");
        break;
    default:
        break;
    }
}

static void to_prompt(ConsoleSession *s)
{
    s->phase = CONSOLE_PH_PROMPT;
    s->onward = CONSOLE_ONWARD_NONE;
    s->typed_len = 0;
    s->hangup_sent = false;
    term_line_init(&s->edit, true);
    show_prompt(s);
}

const char *console_tad_user_name(const ConsoleConfig *cfg, const char *user, char *out,
                                  size_t cap)
{
    size_t n;
    if (cfg == NULL || out == NULL || cap == 0u)
    {
        return NULL;
    }
    if (cfg->tad_user_suffix == NULL || user == NULL || user[0] == '\0')
    {
        if (cfg->tad_user == NULL)
        {
            return NULL;
        }
        (void)copy_text(out, cap, cfg->tad_user);
        return out;
    }
    n = strlen(user);
    if (n + 1u + strlen(cfg->tad_user_suffix) <= CONSOLE_TAD_USER_MAX &&
        n + 1u + strlen(cfg->tad_user_suffix) < cap)
    {
        size_t k = strlen(cfg->tad_user_suffix);
        memcpy(out, user, n);
        out[n] = '@';
        memcpy(&out[n + 1u], cfg->tad_user_suffix, k + 1u); /* with its zero */
        return out;
    }
    if (n > CONSOLE_TAD_USER_MAX)
    {
        n = CONSOLE_TAD_USER_MAX; /* the name alone, cut to what TAD carries */
    }
    if (n >= cap)
    {
        n = cap - 1u;
    }
    memcpy(out, user, n);
    out[n] = '\0';
    return out;
}

/* Start the onward session. Returns false when it could not start (said to the user). */
static bool onward_start(ConsoleSession *s, const char *system, uint32_t now_ms)
{
    const ConsoleServices *sv = &s->host->cfg.services;
    char name[CONSOLE_TAD_USER_MAX + 1u];
    const char *tad_user = console_tad_user_name(&s->host->cfg, s->logged_in ? s->user : NULL,
                                                 name, sizeof name);
    uint16_t number = 0;
    int rc;
    if (!plain_number(system, &number) && sv->resolve(sv->ctx, system, &number) != 0)
    {
        sayf(s, "unknown system %s\r\n", system);
        return false;
    }
    if (!copy_text(s->system, sizeof s->system, system))
    {
        say(s, "that system name is too long\r\n");
        return false;
    }
    s->phase = CONSOLE_PH_SESSION;
    s->onward = CONSOLE_ONWARD_OPENING;
    s->onward_echo = true;
    s->typed_len = 0;
    s->hangup_sent = false;
    term_escape_init(&s->esc,
                     line_is(s, CONSOLE_LINE_ESCAPE_MSG) ? 0u : s->host->cfg.escape_ms);
    (void)now_ms;
    rc = sv->onward_open(sv->ctx, s->place, s->system, number, tad_user);
    if (rc != 0)
    {
        sayf(s, "no free session on the node (%d)\r\n", rc);
        s->onward = CONSOLE_ONWARD_NONE;
        return false;
    }
    XMSG_LOG(XMSG_LOG_CONSOLE, XMSG_LOG_INFO, "place %d (%s): connect %s (system %u)",
             s->place, s->logged_in ? s->user : "direct", s->system, (unsigned)number);
    sayf(s, "connecting to %s...\r\n", s->system);
    return true;
}

static void start_login(ConsoleSession *s)
{
    const ConsoleConfig *c = &s->host->cfg;
    s->phase = CONSOLE_PH_LOGIN;
    term_line_init(&s->edit, true);
    if (auth_login_init(&s->login, &c->store, (c->rng.fill != NULL) ? &c->rng : NULL,
                        c->login_tries, c->login_delay_ms) != 0)
    {
        say(s, "\r\nThe user list cannot be read. Try again later.\r\n");
        close_line(s, CONSOLE_CLOSE_NO_USERS);
        return;
    }
    if (c->banner != NULL)
    {
        say(s, c->banner);
    }
    show_prompt(s);
}

/* "kind peer" for WHO, each part cut so the whole always fits: the kind to 8 characters, the
 * peer to what is left. A cut is visible only as a shorter peer text, never a lost kind. */
static void remote_text(char *out, size_t cap, const char *kind, const char *peer)
{
    size_t k = strlen(kind);
    size_t p = strlen(peer);
    if (k > 8u)
    {
        k = 8u;
    }
    if (k + 2u > cap)
    {
        out[0] = '\0';
        return;
    }
    memcpy(out, kind, k);
    out[k] = ' ';
    if (p > cap - k - 2u)
    {
        p = cap - k - 2u;
    }
    memcpy(&out[k + 1u], peer, p);
    out[k + 1u + p] = '\0';
}

/* A CONNECT argument that is a plain decimal system number, 1..65535: used as it is. Ronny's
 * rule: "a system number or an alias, never a number read out of a name" - so "102" is a
 * number, "D102" is a name and goes through resolve. */
static bool plain_number(const char *text, uint16_t *number)
{
    uint32_t v = 0;
    if (text[0] == '\0')
    {
        return false;
    }
    for (const char *c = text; *c != '\0'; c++)
    {
        if (*c < '0' || *c > '9')
        {
            return false;
        }
        v = (v * 10u) + (uint32_t)(*c - '0');
        if (v > 65535u)
        {
            return false;
        }
    }
    *number = (uint16_t)v;
    return v != 0u;
}

static void start(ConsoleSession *s, const ConsoleLine *line, uint32_t now_ms)
{
    char peer[CONSOLE_REMOTE_MAX + 1u];
    wipe(s);
    s->line = *line;
    s->connected_ms = now_ms;
    s->onward_echo = true;
    peer[0] = '\0';
    if (line->describe != NULL)
    {
        line->describe(line->ctx, peer, sizeof peer);
    }
    remote_text(s->remote, sizeof s->remote, (line->kind != NULL) ? line->kind : "line",
                (peer[0] != '\0') ? peer : "?");
    XMSG_LOG(XMSG_LOG_CONSOLE, XMSG_LOG_INFO, "place %d: on from %s", s->place, s->remote);
    if (s->host->cfg.mode == CONSOLE_MODE_DIRECT)
    {
        if (!onward_start(s, (s->host->cfg.direct_system != NULL) ? s->host->cfg.direct_system
                                                                  : "",
                          now_ms))
        {
            close_line(s, CONSOLE_CLOSE_SESSION_ENDED);
        }
        return;
    }
    if (line_is(s, CONSOLE_LINE_TRUSTED_ADMIN))
    {
        (void)copy_text(s->user, sizeof s->user, AUTH_DEFAULT_ADMIN);
        s->level = AUTH_LEVEL_ADMIN;
        s->logged_in = true;
        if (s->line.logged_in != NULL)
        {
            s->line.logged_in(s->line.ctx, s->user);
        }
        to_prompt(s);
        return;
    }
    start_login(s);
}

/* ------------------------------------------------------------------------------------------ */
/* Host                                                                                       */
/* ------------------------------------------------------------------------------------------ */

void console_config_default(ConsoleConfig *cfg)
{
    if (cfg == NULL)
    {
        return;
    }
    memset(cfg, 0, sizeof *cfg);
    cfg->mode = CONSOLE_MODE_PROMPT;
    cfg->prompt = "TAD> ";
    cfg->iterations = AUTH_DEFAULT_ITERATIONS;
    cfg->escape_ms = TERM_ESCAPE_DEFAULT_MS;
    cfg->login_tries = AUTH_LOGIN_TRIES;
    cfg->login_delay_ms = AUTH_LOGIN_DELAY_MS;
}

int console_host_init(ConsoleHost *h, const ConsoleConfig *cfg, ConsoleSession *sessions,
                      size_t count)
{
    if (h == NULL || cfg == NULL || sessions == NULL || count == 0u ||
        cfg->services.resolve == NULL || cfg->services.onward_open == NULL ||
        cfg->services.onward_write == NULL ||
        cfg->services.onward_hangup == NULL ||
        (cfg->mode == CONSOLE_MODE_PROMPT && cfg->store.lookup == NULL) || cfg->iterations == 0u)
    {
        return -1;
    }
    memset(h, 0, sizeof *h);
    h->cfg = *cfg;
    h->sessions = sessions;
    h->count = count;
    for (size_t i = 0; i < count; i++)
    {
        memset(&sessions[i], 0, sizeof sessions[i]);
        sessions[i].host = h;
        sessions[i].place = (int)i;
    }
    return 0;
}

ConsoleSession *console_host_open_at(ConsoleHost *h, int place, const ConsoleLine *line,
                                     uint32_t now_ms)
{
    ConsoleSession *s;
    if (h == NULL || line == NULL || place < 0 || (size_t)place >= h->count)
    {
        return NULL;
    }
    s = &h->sessions[place];
    if (s->phase != CONSOLE_PH_FREE)
    {
        return NULL;
    }
    start(s, line, now_ms);
    if (s->phase == CONSOLE_PH_FREE)
    {
        return NULL; /* closed at once (no user list, or direct and no session) */
    }
    flush(s);
    return s;
}

ConsoleSession *console_host_open(ConsoleHost *h, const ConsoleLine *line, uint32_t now_ms)
{
    if (h == NULL)
    {
        return NULL;
    }
    for (size_t i = 0; i < h->count; i++)
    {
        if (h->sessions[i].phase == CONSOLE_PH_FREE)
        {
            return console_host_open_at(h, (int)i, line, now_ms);
        }
    }
    return NULL;
}

ConsoleSession *console_host_session(ConsoleHost *h, int place)
{
    if (h == NULL || place < 0 || (size_t)place >= h->count)
    {
        return NULL;
    }
    return &h->sessions[place];
}

bool console_host_place_is_free(const ConsoleHost *h, int place)
{
    if (h == NULL || place < 0 || (size_t)place >= h->count)
    {
        return false;
    }
    return h->sessions[place].phase == CONSOLE_PH_FREE;
}

void console_host_counts(const ConsoleHost *h, uint16_t *in_use, uint16_t *total)
{
    uint16_t n = 0;
    if (h == NULL || in_use == NULL || total == NULL)
    {
        return;
    }
    for (size_t i = 0; i < h->count; i++)
    {
        if (h->sessions[i].phase != CONSOLE_PH_FREE)
        {
            n++;
        }
    }
    *in_use = n;
    *total = (uint16_t)h->count;
}

/* Offer the typed bytes to the onward session; keep what it does not take. */
static void typed_flush(ConsoleSession *s)
{
    const ConsoleServices *sv = &s->host->cfg.services;
    int took;
    if (s->typed_len == 0u || s->onward != CONSOLE_ONWARD_RUNNING)
    {
        return;
    }
    took = sv->onward_write(sv->ctx, s->place, s->typed, s->typed_len);
    if (took <= 0)
    {
        return;
    }
    if ((size_t)took > s->typed_len)
    {
        took = (int)s->typed_len;
    }
    memmove(s->typed, s->typed + took, s->typed_len - (size_t)took);
    s->typed_len -= (size_t)took;
}

static void onward_escape(ConsoleSession *s)
{
    const ConsoleServices *sv = &s->host->cfg.services;
    if (s->onward == CONSOLE_ONWARD_OPENING || s->onward == CONSOLE_ONWARD_RUNNING)
    {
        if (sv->onward_escape != NULL)
        {
            sv->onward_escape(sv->ctx, s->place);
        }
    }
}

void console_host_poll(ConsoleHost *h, uint32_t now_ms)
{
    if (h == NULL)
    {
        return;
    }
    for (size_t i = 0; i < h->count; i++)
    {
        ConsoleSession *s = &h->sessions[i];
        if (s->phase == CONSOLE_PH_LOGIN && auth_login_tick(&s->login, now_ms))
        {
            show_prompt(s);
        }
        if (s->phase == CONSOLE_PH_SESSION)
        {
            if (term_escape_poll(&s->esc, now_ms))
            {
                onward_escape(s); /* a held ESC with nothing after it: the escape key */
            }
            typed_flush(s);
        }
        flush(s);
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Commands                                                                                   */
/* ------------------------------------------------------------------------------------------ */

static void cmd_help(ConsoleSession *s)
{
    say(s, "CONNECT <system>   connect to a SINTRAN system, e.g. CONNECT D102\r\n"
           "LIST               the routing table of the host we are linked to\r\n"
           "NAMES              the system names and named ports the host knows\r\n"
           "WHO                the users on this bridge\r\n"
           "INFO               the link to the SINTRAN machine\r\n"
           "PASSWORD           change your password\r\n"
           "HELP               this text\r\n"
           "EXIT, LOGOUT, QUIT close the connection\r\n");
    if (s->level == AUTH_LEVEL_ADMIN)
    {
        say(s, "USERS              the users (admin)\r\n"
               "ADDUSER name [admin]  add a user (admin)\r\n"
               "DELUSER name       delete a user (admin)\r\n"
               "RESETPW name       set a new password, changed at login (admin)\r\n");
    }
    say(s, "In a session: ESC alone breaks the running program; log out of SINTRAN to\r\n"
           "come back here.\r\n");
}

static void cmd_who(ConsoleSession *self, uint32_t now_ms)
{
    const ConsoleHost *h = self->host;
    char buf[256];
    int head;
    head = term_report_who(NULL, 0, buf, sizeof buf); /* the heading alone */
    if (head <= 0)
    {
        return;
    }
    say(self, buf);
    for (size_t i = 0; i < h->count; i++)
    {
        const ConsoleSession *c = &h->sessions[i];
        TermWhoEntry e;
        if (c->phase == CONSOLE_PH_FREE || c->phase == CONSOLE_PH_DRAIN)
        {
            continue;
        }
        e.terminal = c->place + 1;
        e.user = (h->cfg.mode == CONSOLE_MODE_DIRECT) ? "(direct)"
                                                     : (c->logged_in ? c->user : NULL);
        e.remote = c->remote;
        e.connected_s = (now_ms - c->connected_ms) / 1000u;
        e.system = (c->phase == CONSOLE_PH_SESSION) ? c->system : NULL;
        e.session_state = console_onward_name(c->onward);
        e.is_self = (c == self);
        if (term_report_who(&e, 1, buf, sizeof buf) > head)
        {
            say(self, buf + head); /* the entry's line, without the heading again */
        }
    }
}

static void cmd_info(ConsoleSession *s)
{
    const ConsoleServices *sv = &s->host->cfg.services;
    TermLinkInfo li;
    char buf[1024];
    if (sv->link_info == NULL)
    {
        say(s, "INFO: the link facts are not available here\r\n");
    }
    else
    {
        memset(&li, 0, sizeof li);
        li.peer_node_learned = -1;
        sv->link_info(sv->ctx, &li);
        if (term_report_info(&li, buf, sizeof buf) > 0)
        {
            say(s, buf);
        }
    }
    if (sv->info_more != NULL && sv->info_more(sv->ctx, buf, sizeof buf) > 0)
    {
        buf[sizeof buf - 1u] = '\0';
        say(s, buf);
    }
}

/* Say why a LIST or NAMES walk could not start. Returns true when it did start (rc 0). */
static bool walk_started(ConsoleSession *s, const char *what, int rc)
{
    if (rc == XMSG_ESTATE)
    {
        sayf(s, "%s: the link to the host is not ready\r\n", what);
        return false;
    }
    if (rc == XMSG_ENOSPC)
    {
        sayf(s, "%s: every session slot is in use; try again later\r\n", what);
        return false;
    }
    if (rc == XMSG_EAGAIN)
    {
        sayf(s, "%s: another user's %s is running; try again\r\n", what, what);
        return false;
    }
    if (rc != 0)
    {
        sayf(s, "%s: cannot start (%d)\r\n", what, rc);
        return false;
    }
    return true;
}

/* Returns true when the prompt comes back from console_list_done(). */
static bool cmd_list(ConsoleSession *s)
{
    const ConsoleServices *sv = &s->host->cfg.services;
    TermLinkInfo li;
    if (sv->list_start == NULL)
    {
        say(s, "LIST: not available here\r\n");
        return false;
    }
    if (!walk_started(s, "LIST", sv->list_start(sv->ctx, s->place)))
    {
        return false;
    }
    s->listing = true;
    s->listing_names = false;
    memset(&li, 0, sizeof li);
    if (sv->link_info != NULL)
    {
        sv->link_info(sv->ctx, &li);
    }
    sayf(s, "Routing table of node %u:\r\n"
            "  system  how                   detail  hops  WANs\r\n",
         (unsigned)li.peer_node_cfg);
    return true;
}

/* Walk the user list; `name` set: also find it. Returns the user count, or -1 without a list.
 * `admins_other` receives the admins whose name is not `name`. */
static int users_scan(ConsoleSession *s, const char *name, bool *found, int *admins_other)
{
    const ConsoleUsers *u = &s->host->cfg.users;
    AuthUser rec;
    int n = 0;
    *found = false;
    *admins_other = 0;
    if (u->get == NULL)
    {
        return -1;
    }
    while (u->get(u->ctx, (size_t)n, &rec) == 0)
    {
        bool same = name != NULL && strcmp(rec.name, name) == 0;
        if (same)
        {
            *found = true;
        }
        else if (rec.level == AUTH_LEVEL_ADMIN)
        {
            (*admins_other)++;
        }
        n++;
    }
    auth_wipe(&rec, sizeof rec);
    return n;
}

static void cmd_users(ConsoleSession *s)
{
    const ConsoleUsers *u = &s->host->cfg.users;
    AuthUser rec;
    if (u->get == NULL)
    {
        say(s, "USERS: not available here\r\n");
        return;
    }
    for (size_t i = 0; u->get(u->ctx, i, &rec) == 0; i++)
    {
        sayf(s, "%-32s %-5s%s\r\n", rec.name, (rec.level == AUTH_LEVEL_ADMIN) ? "admin" : "user",
             rec.must_change ? "  must change password" : "");
    }
    auth_wipe(&rec, sizeof rec);
}

static void cmd_deluser(ConsoleSession *s, const char *name)
{
    const ConsoleUsers *u = &s->host->cfg.users;
    AuthUser rec;
    bool found;
    int admins_other;
    bool is_admin;
    if (u->get == NULL || u->remove == NULL)
    {
        say(s, "DELUSER: not available here\r\n");
        return;
    }
    (void)users_scan(s, name, &found, &admins_other);
    if (!found)
    {
        sayf(s, "no user %s\r\n", name);
        return;
    }
    is_admin = s->host->cfg.store.lookup(s->host->cfg.store.ctx, name, &rec) == 0 &&
               rec.level == AUTH_LEVEL_ADMIN;
    auth_wipe(&rec, sizeof rec);
    if (is_admin && admins_other == 0)
    {
        sayf(s, "%s is the last admin and cannot be deleted\r\n", name);
        return;
    }
    if (u->remove(u->ctx, name) != 0)
    {
        say(s, "cannot write the user list\r\n");
        return;
    }
    XMSG_LOG(XMSG_LOG_CONSOLE, XMSG_LOG_INFO, "place %d (%s): deleted user %s", s->place,
             s->user, name);
    sayf(s, "user %s deleted\r\n", name);
}

/* ADDUSER and RESETPW: check the name, then ask the password twice (SETPW). */
static void cmd_setpw_begin(ConsoleSession *s, const char *name, bool add, AuthLevel level)
{
    const AuthUserStore *st = &s->host->cfg.store;
    AuthUser found;
    bool exists;
    if (st->save == NULL || s->host->cfg.rng.fill == NULL)
    {
        say(s, "no password can be set here\r\n");
        return;
    }
    if (!auth_name_is_valid(name))
    {
        sayf(s, "a user name is 1 to %u characters, no blank and no ':'\r\n",
             (unsigned)AUTH_NAME_MAX);
        return;
    }
    exists = st->lookup(st->ctx, name, &found) == 0;
    auth_wipe(&found, sizeof found);
    if (add && exists)
    {
        sayf(s, "user %s exists; RESETPW sets a new password\r\n", name);
        return;
    }
    if (!add && !exists)
    {
        sayf(s, "no user %s\r\n", name);
        return;
    }
    memset(&s->setpw, 0, sizeof s->setpw);
    (void)copy_text(s->setpw.name, sizeof s->setpw.name, name);
    s->setpw.level = level;
    s->setpw_add = add;
    s->setpw_again = false;
    s->phase = CONSOLE_PH_SETPW;
}

/* The first entry in SETPW: make the record (hash only). Returns 0 or -1 (said). */
static int setpw_first(ConsoleSession *s, const char *text)
{
    const ConsoleConfig *c = &s->host->cfg;
    AuthUser u;
    int rc;
    if (text[0] == '\0')
    {
        say(s, "an empty password is not allowed\r\n");
        return -1;
    }
    if (s->setpw_add)
    {
        AuthLevel level = s->setpw.level;
        rc = auth_user_make(&u, s->setpw.name, text, c->iterations, &c->rng);
        u.level = level;
        u.must_change = true; /* the admin knows it; the user picks their own */
    }
    else
    {
        rc = (c->store.lookup(c->store.ctx, s->setpw.name, &u) == 0)
                 ? auth_user_reset_password(&u, text, &c->rng)
                 : -1;
    }
    if (rc != 0)
    {
        say(s, "could not set the password\r\n");
        auth_wipe(&u, sizeof u);
        return -1;
    }
    s->setpw = u;
    auth_wipe(&u, sizeof u);
    s->setpw_again = true;
    return 0;
}

/* A line typed in SETPW. The caller wipes the line. */
static void setpw_line(ConsoleSession *s, const char *text)
{
    const AuthUserStore *st = &s->host->cfg.store;
    if (!s->setpw_again)
    {
        if (setpw_first(s, text) != 0)
        {
            auth_wipe(&s->setpw, sizeof s->setpw);
            s->phase = CONSOLE_PH_PROMPT;
        }
        return;
    }
    if (!auth_user_check(&s->setpw, text))
    {
        say(s, "the two passwords differ; nothing changed\r\n");
    }
    else if (st->save(st->ctx, &s->setpw) != 0)
    {
        say(s, "cannot write the user list\r\n");
    }
    else
    {
        XMSG_LOG(XMSG_LOG_CONSOLE, XMSG_LOG_INFO, "place %d (%s): set the password of %s",
                 s->place, s->user, s->setpw.name);
        sayf(s, "password of %s set; it must be changed at the next login\r\n", s->setpw.name);
    }
    auth_wipe(&s->setpw, sizeof s->setpw);
    s->phase = CONSOLE_PH_PROMPT;
}

/* A line typed in PASSWD (the user's own change, through the login dialogue). */
static void passwd_line(ConsoleSession *s, const char *text, uint32_t now_ms)
{
    AuthLoginState st = auth_login_line(&s->login, text, now_ms);
    AuthLoginNote note = auth_login_take_note(&s->login);
    if (note != AUTH_NOTE_NONE)
    {
        sayf(s, "%s\r\n", auth_login_note_text(note));
    }
    if (note == AUTH_NOTE_CHANGED)
    {
        XMSG_LOG(XMSG_LOG_CONSOLE, XMSG_LOG_INFO, "place %d (%s): changed the password",
                 s->place, s->user);
    }
    if (st == AUTH_LOGIN_DONE)
    {
        s->phase = CONSOLE_PH_PROMPT;
    }
}

static bool admin_only(ConsoleSession *s)
{
    if (s->level == AUTH_LEVEL_ADMIN)
    {
        return true;
    }
    say(s, "not allowed\r\n");
    return false;
}

static void cmd_password(ConsoleSession *s)
{
    if (s->login.state == AUTH_LOGIN_DONE && auth_login_change_begin(&s->login) == 0)
    {
        s->phase = CONSOLE_PH_PASSWD;
        return;
    }
    if (s->login.state != AUTH_LOGIN_DONE)
    {
        say(s, "PASSWORD: this line did not log in with a password\r\n");
        return;
    }
    sayf(s, "%s\r\n", auth_login_note_text(auth_login_take_note(&s->login)));
}

/* The admin commands. */
static void cmd_admin(ConsoleSession *s, TermCmd cmd, const char *arg)
{
    if (!admin_only(s))
    {
        return;
    }
    switch (cmd)
    {
    case TERM_CMD_USERS:
        cmd_users(s);
        break;
    case TERM_CMD_ADDUSER:
        cmd_setpw_begin(s, arg, true, AUTH_LEVEL_USER);
        break;
    case TERM_CMD_ADDADMIN:
        cmd_setpw_begin(s, arg, true, AUTH_LEVEL_ADMIN);
        break;
    case TERM_CMD_DELUSER:
        cmd_deluser(s, arg);
        break;
    default: /* TERM_CMD_RESETPW */
        cmd_setpw_begin(s, arg, false, AUTH_LEVEL_USER);
        break;
    }
}

/* Returns true when the prompt comes back from console_names_done(). */
static bool cmd_names(ConsoleSession *s)
{
    const ConsoleServices *sv = &s->host->cfg.services;
    TermLinkInfo li;
    if (sv->names_start == NULL)
    {
        say(s, "NAMES: not available here\r\n");
        return false;
    }
    if (!walk_started(s, "NAMES", sv->names_start(sv->ctx, s->place)))
    {
        return false;
    }
    s->listing = true;
    s->listing_names = true;
    memset(&li, 0, sizeof li);
    if (sv->link_info != NULL)
    {
        sv->link_info(sv->ctx, &li);
    }
    /* The port column is the port word >> 7, as LIST-NAMES prints it (0x0401 -> 8). */
    sayf(s, "Names known to node %u:\r\n"
            "  name              system  port  free\r\n",
         (unsigned)li.peer_node_cfg);
    return true;
}

/* One line typed at the prompt. */
/* size exemption: command table */
static void prompt_line(ConsoleSession *s, const char *text, uint32_t now_ms)
{
    char arg[AUTH_NAME_MAX + 1u];
    TermCmd cmd = term_cmd_parse(text, arg, sizeof arg);
    if (s->listing)
    {
        say(s, s->listing_names ? "NAMES is still running\r\n" : "LIST is still running\r\n");
        return;
    }
    switch (cmd)
    {
    case TERM_CMD_EMPTY:
        break;
    case TERM_CMD_CONNECT:
        if (strlen(arg) > CONSOLE_SYSTEM_MAX)
        {
            say(s, "that system name is too long\r\n");
            break;
        }
        if (onward_start(s, arg, now_ms))
        {
            return; /* no prompt: the host's banner comes */
        }
        s->phase = CONSOLE_PH_PROMPT;
        break;
    case TERM_CMD_LIST:
        if (cmd_list(s))
        {
            return; /* the prompt comes back when the walk ends */
        }
        break;
    case TERM_CMD_NAMES:
        if (cmd_names(s))
        {
            return; /* the prompt comes back when the walk ends */
        }
        break;
    case TERM_CMD_WHO:
        cmd_who(s, now_ms);
        break;
    case TERM_CMD_INFO:
        cmd_info(s);
        break;
    case TERM_CMD_HELP:
        cmd_help(s);
        break;
    case TERM_CMD_EXIT:
        say(s, "bye\r\n");
        close_line(s, CONSOLE_CLOSE_EXIT);
        return;
    case TERM_CMD_PASSWORD:
        cmd_password(s);
        break;
    case TERM_CMD_USERS:
        /* fall through */
    case TERM_CMD_ADDUSER:
        /* fall through */
    case TERM_CMD_ADDADMIN:
        /* fall through */
    case TERM_CMD_DELUSER:
        /* fall through */
    case TERM_CMD_RESETPW:
        cmd_admin(s, cmd, arg);
        break;
    default:
        say(s, "unknown command; HELP lists them\r\n");
        break;
    }
    show_prompt(s);
}

/* A record kept at another PBKDF2 count is stored again at the configured one, from the
 * password just typed: a password change keeps the record's own count (auth_user.c), so it
 * would otherwise never move (XMSGGW: 100000 rounds took 22.8 s on an ESP32-S2). */
static void restore_count(ConsoleSession *s, const char *password)
{
    const ConsoleConfig *c = &s->host->cfg;
    const AuthUser *rec = auth_login_user(&s->login);
    AuthUser fresh;
    if (rec == NULL || rec->iterations == c->iterations || c->store.save == NULL ||
        c->rng.fill == NULL || password[0] == '\0')
    {
        return;
    }
    if (auth_user_make(&fresh, rec->name, password, c->iterations, &c->rng) == 0)
    {
        fresh.level = rec->level;
        fresh.must_change = rec->must_change;
        /* The save is its own statement: XMSG_LOG does not evaluate its arguments when off. */
        bool saved = c->store.save(c->store.ctx, &fresh) == 0;
        XMSG_LOG(XMSG_LOG_CONSOLE, XMSG_LOG_INFO, "place %d: %s stored again at %lu rounds (was "
                 "%lu): %s", s->place, rec->name, (unsigned long)c->iterations,
                 (unsigned long)rec->iterations, saved ? "ok" : "write failed");
    }
    auth_wipe(&fresh, sizeof fresh);
}

static void logged_in(ConsoleSession *s, AuthLoginNote note)
{
    (void)copy_text(s->user, sizeof s->user, s->login.name);
    s->level = auth_login_level(&s->login);
    s->logged_in = true;
    XMSG_LOG(XMSG_LOG_CONSOLE, XMSG_LOG_INFO, "place %d: logged in as %s (%s)%s", s->place,
             s->user, (s->level == AUTH_LEVEL_ADMIN) ? "admin" : "user",
             (note == AUTH_NOTE_CHANGED) ? ", new password stored" : "");
    say(s, "Logged in. HELP lists the commands.\r\n");
    if (s->line.logged_in != NULL)
    {
        s->line.logged_in(s->line.ctx, s->user);
    }
    to_prompt(s);
}

/* One line typed at the login. The line (maybe a password) is wiped by the caller. */
static void login_line(ConsoleSession *s, const char *text, uint32_t now_ms)
{
    AuthLoginState before = s->login.state;
    AuthLoginState st = auth_login_line(&s->login, text, now_ms);
    AuthLoginNote note = auth_login_take_note(&s->login);
    if (note != AUTH_NOTE_NONE)
    {
        sayf(s, "%s\r\n", auth_login_note_text(note));
    }
    if (st == AUTH_LOGIN_NEW_PASSWORD && before == AUTH_LOGIN_PASSWORD)
    {
        say(s, "You must set a new password before going on.\r\n");
    }
    switch (st)
    {
    case AUTH_LOGIN_DONE:
        if (before == AUTH_LOGIN_PASSWORD || before == AUTH_LOGIN_NEW_AGAIN)
        {
            restore_count(s, text); /* the text is the password now in force */
        }
        logged_in(s, note);
        break;
    case AUTH_LOGIN_WAIT:
        XMSG_LOG(XMSG_LOG_CONSOLE, XMSG_LOG_WARN, "place %d: login failed", s->place);
        say(s, "Login incorrect\r\n");
        break;
    case AUTH_LOGIN_REFUSED:
        XMSG_LOG(XMSG_LOG_CONSOLE, XMSG_LOG_WARN, "place %d: login refused", s->place);
        if (note == AUTH_NOTE_NONE)
        {
            say(s, "Login incorrect\r\n");
        }
        close_line(s, CONSOLE_CLOSE_LOGIN_FAILED);
        break;
    default:
        show_prompt(s);
        break;
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Input                                                                                      */
/* ------------------------------------------------------------------------------------------ */

/* A byte for the command line or the login. */
static void line_byte(ConsoleSession *s, uint8_t k, uint32_t now_ms)
{
    uint8_t echo[3];
    size_t el = 0;
    char text[TERM_LINE_MAX + 1u];
    if (s->phase == CONSOLE_PH_LOGIN && s->login.state == AUTH_LOGIN_WAIT)
    {
        return; /* typed while a failed login waits: thrown away */
    }
    if (term_line_byte(&s->edit, k, echo, &el) != 1)
    {
        out_raw(s, echo, el);
        return;
    }
    out_raw(s, echo, el);
    memcpy(text, s->edit.text, sizeof text);
    term_line_clear(&s->edit);
    switch (s->phase)
    {
    case CONSOLE_PH_LOGIN:
        login_line(s, text, now_ms);
        break;
    case CONSOLE_PH_PASSWD:
        passwd_line(s, text, now_ms);
        show_prompt(s);
        break;
    case CONSOLE_PH_SETPW:
        setpw_line(s, text);
        show_prompt(s);
        break;
    default:
        prompt_line(s, text, now_ms);
        break;
    }
    auth_wipe(text, sizeof text);
}

/* Local echo of a byte typed in a session, on a line where we echo. Not for the Enter key: the
 * host sends its own CR LF for it (XMSGGW, Ronny's log 2026-11-09 23:37:39: echoing CR gave two
 * new lines per Enter); not for ESC (a function key's start); BS and DEL take the character
 * back on the screen. host_tad_telnet echoed every byte; this is the rule measured live since. */
static void session_echo(ConsoleSession *s, uint8_t b)
{
    static const uint8_t back[] = {BS, ' ', BS};
    if (line_is(s, CONSOLE_LINE_OWN_ECHO) || !s->onward_echo || b == TERM_ESC || b == CR)
    {
        return;
    }
    if (b == BS || b == DEL)
    {
        out_raw(s, back, sizeof back);
        return;
    }
    out_raw(s, &b, 1);
}

/* A byte for the onward session: the lone-ESC rule, then queued in order. */
static void session_byte(ConsoleSession *s, uint8_t k, uint32_t now_ms)
{
    uint8_t out[2];
    bool escape = false;
    int n = term_escape_byte(&s->esc, k, now_ms, out, &escape);
    if (escape)
    {
        onward_escape(s);
    }
    for (int i = 0; i < n; i++)
    {
        if (s->typed_len >= sizeof s->typed)
        {
            return; /* the host is not taking input; the user typed far ahead of it */
        }
        s->typed[s->typed_len++] = out[i];
        session_echo(s, out[i]);
    }
}

void console_input(ConsoleSession *s, const uint8_t *bytes, size_t len, uint32_t now_ms)
{
    if (s == NULL || (bytes == NULL && len != 0u))
    {
        return;
    }
    for (size_t i = 0; i < len; i++)
    {
        if (s->phase == CONSOLE_PH_SESSION)
        {
            session_byte(s, bytes[i], now_ms);
        }
        else if (s->phase == CONSOLE_PH_FREE || s->phase == CONSOLE_PH_DRAIN)
        {
            return; /* closed by the line just typed (EXIT): the rest is not ours */
        }
        else
        {
            line_byte(s, bytes[i], now_ms);
        }
    }
    if (s->phase == CONSOLE_PH_SESSION)
    {
        typed_flush(s);
    }
    flush(s);
}

void console_escape(ConsoleSession *s)
{
    if (s != NULL && s->phase == CONSOLE_PH_SESSION)
    {
        onward_escape(s);
    }
}

void console_line_lost(ConsoleSession *s)
{
    if (s == NULL || s->phase == CONSOLE_PH_FREE || s->phase == CONSOLE_PH_DRAIN)
    {
        return;
    }
    XMSG_LOG(XMSG_LOG_CONSOLE, XMSG_LOG_INFO, "place %d: the line is gone", s->place);
    walk_cancel(s);
    if (s->phase == CONSOLE_PH_SESSION &&
        (s->onward == CONSOLE_ONWARD_OPENING || s->onward == CONSOLE_ONWARD_RUNNING))
    {
        /* Hang up as a real asker does when its user is done; the place is free once the
         * onward session has ended (console_onward_state). */
        bool sent = s->hangup_sent;
        ConsoleHost *h = s->host;
        int place = s->place;
        wipe(s);
        s->phase = CONSOLE_PH_DRAIN;
        if (!sent)
        {
            h->cfg.services.onward_hangup(h->cfg.services.ctx, place);
        }
        return;
    }
    wipe(s);
}

/* ------------------------------------------------------------------------------------------ */
/* Feeds from the onward session                                                              */
/* ------------------------------------------------------------------------------------------ */

void console_onward_text(ConsoleSession *s, const uint8_t *bytes, size_t len)
{
    if (s == NULL || bytes == NULL || s->phase != CONSOLE_PH_SESSION)
    {
        return;
    }
    out_raw(s, bytes, len);
    flush(s);
}

void console_onward_state(ConsoleSession *s, ConsoleOnward st, int error)
{
    if (s == NULL)
    {
        return;
    }
    if (s->phase == CONSOLE_PH_DRAIN)
    {
        if (st == CONSOLE_ONWARD_CLOSED || st == CONSOLE_ONWARD_FAILED)
        {
            wipe(s); /* the user went earlier; now the place is free */
        }
        return;
    }
    if (s->phase != CONSOLE_PH_SESSION)
    {
        return;
    }
    if (st == CONSOLE_ONWARD_RUNNING)
    {
        s->onward = CONSOLE_ONWARD_RUNNING;
        typed_flush(s);
        flush(s);
        return;
    }
    if (st != CONSOLE_ONWARD_CLOSED && st != CONSOLE_ONWARD_FAILED)
    {
        return;
    }
    if (error != 0)
    {
        sayf(s, "\r\n<session %s, error %d>\r\n", console_onward_name(st), error);
    }
    else
    {
        sayf(s, "\r\n<session %s>\r\n", console_onward_name(st));
    }
    XMSG_LOG(XMSG_LOG_CONSOLE, XMSG_LOG_INFO, "place %d: session to %s %s", s->place, s->system,
             console_onward_name(st));
    if (s->host->cfg.mode == CONSOLE_MODE_DIRECT)
    {
        close_line(s, CONSOLE_CLOSE_SESSION_ENDED);
        return;
    }
    to_prompt(s);
    flush(s);
}

void console_onward_echo(ConsoleSession *s, bool on)
{
    if (s == NULL || s->phase != CONSOLE_PH_SESSION)
    {
        return;
    }
    s->onward_echo = on;
    if (line_is(s, CONSOLE_LINE_OWN_ECHO))
    {
        set_echo(s, on); /* the caller's own machine echoes: tell it to stop for a password */
        flush(s);
    }
}

void console_onward_logout(ConsoleSession *s)
{
    if (s == NULL || s->phase != CONSOLE_PH_SESSION || s->hangup_sent ||
        (s->onward != CONSOLE_ONWARD_OPENING && s->onward != CONSOLE_ONWARD_RUNNING))
    {
        return;
    }
    XMSG_LOG(XMSG_LOG_CONSOLE, XMSG_LOG_INFO, "place %d: the SINTRAN login ended", s->place);
    s->hangup_sent = true;
    s->host->cfg.services.onward_hangup(s->host->cfg.services.ctx, s->place);
}

void console_list_entry(ConsoleSession *s, const XroutRouteEntry *e)
{
    if (s == NULL || e == NULL || !s->listing || s->listing_names)
    {
        return;
    }
    sayf(s, "  %6u  %u %-19s %6u  %4u  %4u\r\n", (unsigned)e->system, (unsigned)e->how,
         xrout_route_how_name(e->how), (unsigned)e->detail, (unsigned)xrout_route_hops(e),
         (unsigned)xrout_route_wans(e));
}

void console_list_done(ConsoleSession *s, int result, unsigned entries)
{
    if (s == NULL || !s->listing || s->listing_names)
    {
        return;
    }
    s->listing = false;
    if (result == XMSG_OK)
    {
        sayf(s, "%u system(s)\r\n", entries);
    }
    else if (result == XMSG_ETIMEDOUT)
    {
        sayf(s, "LIST: the host did not answer (after %u system(s))\r\n", entries);
    }
    else
    {
        sayf(s, "LIST: the host refused the question (error %d, after %u system(s))\r\n",
             result, entries);
    }
    show_prompt(s);
    flush(s);
}

void console_names_row(ConsoleSession *s, const XroutNameRow *row)
{
    if (s == NULL || row == NULL || !s->listing || !s->listing_names)
    {
        return;
    }
    if (xrout_name_is_system(row))
    {
        sayf(s, "  %-16s  %6u  system\r\n", row->name, (unsigned)row->system);
    }
    else if (row->free == XROUT_NAME_NO_FREE)
    {
        sayf(s, "  %-16s  %6u  %4u\r\n", row->name, (unsigned)row->system,
             (unsigned)(row->port >> 7u));
    }
    else
    {
        sayf(s, "  %-16s  %6u  %4u  %4ld\r\n", row->name, (unsigned)row->system,
             (unsigned)(row->port >> 7u), (long)row->free);
    }
}

void console_names_done(ConsoleSession *s, int result, unsigned rows)
{
    if (s == NULL || !s->listing || !s->listing_names)
    {
        return;
    }
    s->listing = false;
    s->listing_names = false;
    if (result == XMSG_OK)
    {
        sayf(s, "%u name(s)\r\n", rows);
    }
    else if (result == XMSG_ETIMEDOUT)
    {
        sayf(s, "NAMES: the host did not answer (after %u name(s))\r\n", rows);
    }
    else
    {
        sayf(s, "NAMES: the host refused the question (error %d, after %u name(s))\r\n",
             result, rows);
    }
    show_prompt(s);
    flush(s);
}

const char *console_onward_name(ConsoleOnward st)
{
    switch (st)
    {
    case CONSOLE_ONWARD_NONE:
        return "none";
    case CONSOLE_ONWARD_OPENING:
        return "opening";
    case CONSOLE_ONWARD_RUNNING:
        return "running";
    case CONSOLE_ONWARD_CLOSED:
        return "closed";
    case CONSOLE_ONWARD_FAILED:
        return "failed";
    default:
        return "?";
    }
}
