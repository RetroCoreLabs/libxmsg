/*
 * test_console.c - the login and command line, driven through a fake line and a fake node.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * The fake line keeps every byte the core writes and every call it makes; the fake services
 * port keeps every onward and LIST request. Users live in an in-memory store with a tiny
 * PBKDF2 count so the test runs fast.
 */
#include "libxmsg/console/console.h"
#include "libxmsg/xmsg_errors.h"

#include "test_util.h"

/* ------------------------------------------------------------------ fakes */

typedef struct FakeLine
{
    char out[8192];
    size_t len;
    unsigned flushes;
    int echo_calls;
    bool echo_last;
    unsigned logins;
    int closed; /* the ConsoleClose reason; 0 = open */
} FakeLine;

static int fl_write(void *ctx, const uint8_t *b, size_t n)
{
    FakeLine *f = (FakeLine *)ctx;
    if (f->len + n >= sizeof f->out)
    {
        return 0;
    }
    memcpy(f->out + f->len, b, n);
    f->len += n;
    f->out[f->len] = '\0';
    return (int)n;
}

static void fl_flush(void *ctx)
{
    ((FakeLine *)ctx)->flushes++;
}

static void fl_echo(void *ctx, bool on)
{
    ((FakeLine *)ctx)->echo_calls++;
    ((FakeLine *)ctx)->echo_last = on;
}

static void fl_logged_in(void *ctx, const char *user)
{
    (void)user;
    ((FakeLine *)ctx)->logins++;
}

static void fl_close(void *ctx, ConsoleClose why)
{
    ((FakeLine *)ctx)->closed = (int)why;
}

static void fl_describe(void *ctx, char *buf, size_t cap)
{
    (void)ctx;
    (void)snprintf(buf, cap, "1.2.3.4:5");
}

static ConsoleLine make_line(FakeLine *f, uint32_t caps)
{
    ConsoleLine l;
    memset(f, 0, sizeof *f);
    memset(&l, 0, sizeof l);
    l.ctx = f;
    l.caps = caps;
    l.kind = "telnet";
    l.write = fl_write;
    l.flush = fl_flush;
    l.echo = fl_echo;
    l.logged_in = fl_logged_in;
    l.close = fl_close;
    l.describe = fl_describe;
    return l;
}

static bool has(const FakeLine *f, const char *text)
{
    return strstr(f->out, text) != NULL;
}

static void clear(FakeLine *f)
{
    f->len = 0;
    f->out[0] = '\0';
}

typedef struct FakeNode
{
    int opens;
    int open_rc;
    char system[16];
    uint16_t number;
    char tad_user[32];
    bool tad_user_null;
    uint8_t typed[512];
    size_t typed_len;
    int take;        /* how many bytes onward_write takes; -1 = all */
    unsigned writes; /* onward_write calls that took bytes */
    int escapes;
    int hangups;
    int list_rc;
    int lists;
    int list_cancels;
    int names_rc;
    int names;
    int names_system; /* the system the last NAMES asked (0: the linked host) */
    int names_cancels;
} FakeNode;

static int fn_resolve(void *ctx, const char *system, uint16_t *number)
{
    (void)ctx;
    if (strcmp(system, "D100") == 0)
    {
        *number = 100;
        return 0;
    }
    if (strcmp(system, "D102") == 0)
    {
        *number = 102;
        return 0;
    }
    return -1;
}

static int fn_open(void *ctx, int place, const char *system, uint16_t number, const char *tad_user)
{
    FakeNode *n = (FakeNode *)ctx;
    (void)place;
    n->opens++;
    (void)snprintf(n->system, sizeof n->system, "%s", system);
    n->number = number;
    n->tad_user_null = (tad_user == NULL);
    (void)snprintf(n->tad_user, sizeof n->tad_user, "%s", (tad_user != NULL) ? tad_user : "");
    return n->open_rc;
}

static int fn_write(void *ctx, int place, const uint8_t *b, size_t len)
{
    FakeNode *n = (FakeNode *)ctx;
    size_t k = (n->take < 0) ? len : ((size_t)n->take < len ? (size_t)n->take : len);
    (void)place;
    memcpy(n->typed + n->typed_len, b, k);
    n->typed_len += k;
    if (k != 0u)
    {
        n->writes++;
    }
    return (int)k;
}

static void fn_escape(void *ctx, int place)
{
    (void)place;
    ((FakeNode *)ctx)->escapes++;
}

static void fn_hangup(void *ctx, int place)
{
    (void)place;
    ((FakeNode *)ctx)->hangups++;
}

static int fn_list(void *ctx, int place)
{
    (void)place;
    ((FakeNode *)ctx)->lists++;
    return ((FakeNode *)ctx)->list_rc;
}

static void fn_list_cancel(void *ctx, int place)
{
    (void)place;
    ((FakeNode *)ctx)->list_cancels++;
}

static int fn_names(void *ctx, int place, uint16_t system)
{
    (void)place;
    ((FakeNode *)ctx)->names_system = system;
    ((FakeNode *)ctx)->names++;
    return ((FakeNode *)ctx)->names_rc;
}

static void fn_names_cancel(void *ctx, int place)
{
    (void)place;
    ((FakeNode *)ctx)->names_cancels++;
}

static void fn_info(void *ctx, TermLinkInfo *li)
{
    (void)ctx;
    li->own_node = 19999;
    li->own_system = 19999;
    li->peer_node_cfg = 100;
    li->peer_system = "D100";
    li->link_up = true;
    li->link_ready = true;
    li->transport = "test";
}

#define USERS 8

typedef struct Store
{
    AuthUser u[USERS];
    size_t n;
    unsigned saves;
} Store;

static int st_lookup(void *ctx, const char *name, AuthUser *out)
{
    Store *s = (Store *)ctx;
    for (size_t i = 0; i < s->n; i++)
    {
        if (strcmp(s->u[i].name, name) == 0)
        {
            *out = s->u[i];
            return 0;
        }
    }
    return -1;
}

static int st_save(void *ctx, const AuthUser *u)
{
    Store *s = (Store *)ctx;
    s->saves++;
    for (size_t i = 0; i < s->n; i++)
    {
        if (strcmp(s->u[i].name, u->name) == 0)
        {
            s->u[i] = *u;
            return 0;
        }
    }
    if (s->n >= USERS)
    {
        return -1;
    }
    s->u[s->n++] = *u;
    return 0;
}

static int st_get(void *ctx, size_t i, AuthUser *out)
{
    Store *s = (Store *)ctx;
    if (i >= s->n)
    {
        return -1;
    }
    *out = s->u[i];
    return 0;
}

static int st_remove(void *ctx, const char *name)
{
    Store *s = (Store *)ctx;
    for (size_t i = 0; i < s->n; i++)
    {
        if (strcmp(s->u[i].name, name) == 0)
        {
            s->u[i] = s->u[s->n - 1u];
            s->n--;
            return 0;
        }
    }
    return -1;
}

static int rng_fill(void *ctx, uint8_t *buf, size_t len)
{
    static uint8_t k;
    (void)ctx;
    for (size_t i = 0; i < len; i++)
    {
        buf[i] = k++;
    }
    return 0;
}

#define ROUNDS 2u

static Store s_store;
static FakeNode s_node;
static ConsoleHost s_host;
static ConsoleSession s_sessions[3];

static void add_user(const char *name, const char *pw, AuthLevel level, bool change,
                     uint32_t rounds)
{
    AuthRandom rng = {NULL, rng_fill};
    AuthUser u;
    CHECK_EQ(auth_user_make(&u, name, pw, rounds, &rng), 0);
    u.level = level;
    u.must_change = change;
    s_store.u[s_store.n++] = u;
}

static void setup(ConsoleMode mode)
{
    ConsoleConfig cfg;
    memset(&s_store, 0, sizeof s_store);
    memset(&s_node, 0, sizeof s_node);
    s_node.take = -1;
    add_user("alice", "pw", AUTH_LEVEL_ADMIN, false, ROUNDS);
    add_user("bob", "old", AUTH_LEVEL_USER, true, ROUNDS);
    add_user("carol", "cpw", AUTH_LEVEL_USER, false, 3u); /* another count: re-stored */
    console_config_default(&cfg);
    cfg.mode = mode;
    cfg.banner = "BRIDGE\r\n";
    cfg.direct_system = "D100";
    cfg.tad_user = "TELNET";
    cfg.iterations = ROUNDS;
    cfg.login_delay_ms = 1000;
    cfg.store.ctx = &s_store;
    cfg.store.lookup = st_lookup;
    cfg.store.save = st_save;
    cfg.rng.fill = rng_fill;
    cfg.users.ctx = &s_store;
    cfg.users.get = st_get;
    cfg.users.remove = st_remove;
    cfg.services.ctx = &s_node;
    cfg.services.resolve = fn_resolve;
    cfg.services.onward_open = fn_open;
    cfg.services.onward_write = fn_write;
    cfg.services.onward_escape = fn_escape;
    cfg.services.onward_hangup = fn_hangup;
    cfg.services.list_start = fn_list;
    cfg.services.list_cancel = fn_list_cancel;
    cfg.services.names_start = fn_names;
    cfg.services.names_cancel = fn_names_cancel;
    cfg.services.link_info = fn_info;
    CHECK_EQ(console_host_init(&s_host, &cfg, s_sessions, 3), 0);
}

static void type(ConsoleSession *s, const char *text, uint32_t now)
{
    console_input(s, (const uint8_t *)text, strlen(text), now);
}

/* Log in as `name`; returns the session at the prompt. */
static ConsoleSession *login(FakeLine *f, const ConsoleLine *l, const char *name, const char *pw)
{
    char line[64];
    ConsoleSession *s = console_host_open(&s_host, l, 0);
    CHECK(s != NULL);
    (void)snprintf(line, sizeof line, "%s\r", name);
    type(s, line, 1);
    (void)snprintf(line, sizeof line, "%s\r", pw);
    type(s, line, 2);
    CHECK(has(f, "Logged in."));
    clear(f);
    return s;
}

/* ------------------------------------------------------------------ tests */

static void login_ok_and_bad(void)
{
    FakeLine f;
    ConsoleLine l = make_line(&f, 0);
    ConsoleSession *s;
    uint16_t used = 0;
    uint16_t total = 0;
    setup(CONSOLE_MODE_PROMPT);

    s = console_host_open(&s_host, &l, 0);
    CHECK(s != NULL && s->place == 0);
    CHECK(strcmp(f.out, "BRIDGE\r\nlogin: ") == 0);
    CHECK(f.flushes >= 1);
    clear(&f);
    type(s, "alice\r", 1);
    CHECK(strcmp(f.out, "alice\r\npassword: ") == 0);
    clear(&f);
    type(s, "pw\r", 2);
    CHECK(strcmp(f.out, "\r\nLogged in. HELP lists the commands.\r\nTAD> ") == 0); /* no echo */
    CHECK_EQ(f.logins, 1);
    console_host_counts(&s_host, &used, &total);
    CHECK_EQ(used, 1);
    CHECK_EQ(total, 3);

    /* Bad: three tries, typing during the wait is thrown away, then the line is closed. */
    {
        FakeLine g;
        ConsoleLine m = make_line(&g, 0);
        ConsoleSession *t = console_host_open(&s_host, &m, 0);
        uint32_t now = 10;
        CHECK(t != NULL && t->place == 1);
        for (int i = 0; i < 3; i++)
        {
            clear(&g);
            type(t, "alice\r", now);
            type(t, "nope\r", now);
            CHECK(has(&g, "Login incorrect"));
            type(t, "x\r", now); /* during the wait */
            now += 1000;
            console_host_poll(&s_host, now);
        }
        CHECK_EQ(g.closed, CONSOLE_CLOSE_LOGIN_FAILED);
        CHECK(console_host_place_is_free(&s_host, 1));
    }
    (void)s;
}

static void must_change_and_restore(void)
{
    FakeLine f;
    ConsoleLine l = make_line(&f, 0);
    ConsoleSession *s;
    AuthUser u;
    setup(CONSOLE_MODE_PROMPT);

    s = console_host_open(&s_host, &l, 0);
    type(s, "bob\rold\r", 1);
    CHECK(has(&f, "You must set a new password before going on."));
    CHECK(has(&f, "new password: "));
    clear(&f);
    type(s, "new\r", 2);
    CHECK(has(&f, "again: "));
    type(s, "new\r", 3);
    CHECK(has(&f, "Logged in."));
    CHECK_EQ(st_lookup(&s_store, "bob", &u), 0);
    CHECK(!u.must_change);
    CHECK(auth_user_check(&u, "new"));

    /* carol is kept at 3 rounds; the login stores her again at 2, same password. */
    {
        FakeLine g;
        ConsoleLine m = make_line(&g, 0);
        (void)login(&g, &m, "carol", "cpw");
        CHECK_EQ(st_lookup(&s_store, "carol", &u), 0);
        CHECK_EQ(u.iterations, ROUNDS);
        CHECK(auth_user_check(&u, "cpw"));
    }
}

static void commands(void)
{
    FakeLine f;
    ConsoleLine l = make_line(&f, 0);
    ConsoleSession *s;
    XroutRouteEntry e = {102, 1, 0, 0x0001};
    AuthUser u;
    setup(CONSOLE_MODE_PROMPT);
    s = login(&f, &l, "alice", "pw");

    type(s, "help\r", 3);
    CHECK(has(&f, "CONNECT <system>") && has(&f, "DELUSER name"));
    clear(&f);
    type(s, "who\r", 3);
    CHECK(has(&f, "TERM USER") && has(&f, "*    1 alice") && has(&f, "telnet 1.2.3.4:5"));
    clear(&f);
    type(s, "info\r", 3);
    CHECK(has(&f, "node 19999") && has(&f, "D100"));
    clear(&f);

    type(s, "list\r", 3);
    CHECK(has(&f, "Routing table of node 100:"));
    CHECK(!has(&f, "TAD> "));
    type(s, "who\r", 3);
    CHECK(has(&f, "LIST is still running"));
    console_list_entry(s, &e);
    console_list_done(s, XMSG_OK, 1);
    CHECK(has(&f, "     102") && has(&f, "1 system(s)\r\nTAD> "));
    clear(&f);
    s_node.list_rc = XMSG_EAGAIN;
    type(s, "list\r", 3);
    CHECK(has(&f, "another user's LIST is running"));
    clear(&f);

    /* NAMES: rows from the captured D100 table (listnames-hub, 2026-11-10). */
    XroutNameRow row;
    type(s, "names\r", 3);
    CHECK_EQ(s_node.names, 1);
    CHECK(has(&f, "Names known to node 100:"));
    CHECK(!has(&f, "TAD> "));
    type(s, "who\r", 3);
    CHECK(has(&f, "NAMES is still running"));
    /* A LIST feed while NAMES runs is not shown. */
    console_list_entry(s, &e);
    CHECK(!has(&f, "     102"));
    CHECK_EQ(xrout_name_row_set(&row, 100, 0x0001, "D100", XROUT_NAME_NO_FREE), XMSG_OK);
    console_names_row(s, &row);
    CHECK_EQ(xrout_name_row_set(&row, 100, 0x0401, "*TADADM", XROUT_NAME_NO_FREE), XMSG_OK);
    console_names_row(s, &row);
    CHECK_EQ(xrout_name_row_set(&row, 100, 0x04CE, "*FA-FSA", 2), XMSG_OK);
    console_names_row(s, &row);
    console_names_done(s, XMSG_OK, 3);
    CHECK(has(&f, "  D100                 100  system\r\n"));
    CHECK(has(&f, "  *TADADM              100     8\r\n"));
    CHECK(has(&f, "  *FA-FSA              100     9     2\r\n"));
    CHECK(has(&f, "3 name(s)\r\nTAD> "));
    clear(&f);
    console_names_done(s, XMSG_OK, 3); /* not running: nothing */
    CHECK(!has(&f, "name(s)"));
    s_node.names_rc = XMSG_ESTATE;
    type(s, "names\r", 3);
    CHECK(has(&f, "NAMES: the link to the host is not ready"));
    clear(&f);
    s_node.names_rc = 0;
    type(s, "names\r", 3);
    console_names_done(s, XMSG_ETIMEDOUT, 0);
    CHECK(has(&f, "NAMES: the host did not answer (after 0 name(s))"));
    clear(&f);
    /* A user who goes away during NAMES has the NAMES walk stopped, not the LIST one. */
    type(s, "names\r", 3);
    int list_cancels = s_node.list_cancels;
    console_line_lost(s);
    CHECK_EQ(s_node.names_cancels, 1);
    CHECK_EQ(s_node.list_cancels, list_cancels);
    s = login(&f, &l, "alice", "pw");
    clear(&f);

    type(s, "users\r", 3);
    CHECK(has(&f, "bob") && has(&f, "must change password"));
    clear(&f);
    type(s, "adduser dave\r", 3);
    CHECK(has(&f, "new password for dave: "));
    type(s, "d1\rd1\r", 3);
    CHECK(has(&f, "password of dave set"));
    CHECK_EQ(st_lookup(&s_store, "dave", &u), 0);
    CHECK(u.must_change && u.level == AUTH_LEVEL_USER && u.iterations == ROUNDS);
    clear(&f);
    type(s, "adduser erin admin\re1\re2\r", 3);
    CHECK(has(&f, "the two passwords differ; nothing changed"));
    CHECK(st_lookup(&s_store, "erin", &u) != 0);
    clear(&f);
    type(s, "resetpw bob\rr1\rr1\r", 3);
    CHECK_EQ(st_lookup(&s_store, "bob", &u), 0);
    CHECK(auth_user_check(&u, "r1") && u.must_change);
    clear(&f);
    type(s, "deluser alice\r", 3);
    CHECK(has(&f, "alice is the last admin"));
    type(s, "deluser dave\r", 3);
    CHECK(has(&f, "user dave deleted"));
    CHECK(st_lookup(&s_store, "dave", &u) != 0);
    clear(&f);
    type(s, "password\rpw\rp2\rp2\r", 3);
    CHECK(has(&f, "old password: ") && has(&f, "new password: "));
    CHECK_EQ(st_lookup(&s_store, "alice", &u), 0);
    CHECK(auth_user_check(&u, "p2"));
    clear(&f);
    type(s, "frobnicate\r", 3);
    CHECK(has(&f, "unknown command"));
    type(s, "connect D999\r", 3);
    CHECK(has(&f, "unknown system D999"));
    CHECK_EQ(s_node.opens, 0);
    type(s, "exit\r", 3);
    CHECK(has(&f, "bye\r\n"));
    CHECK_EQ(f.closed, CONSOLE_CLOSE_EXIT);
    CHECK(console_host_place_is_free(&s_host, 0));

    /* A user may not run the admin commands. */
    {
        FakeLine g;
        ConsoleLine m = make_line(&g, 0);
        ConsoleSession *t = login(&g, &m, "carol", "cpw");
        type(t, "users\r", 3);
        CHECK(has(&g, "not allowed"));
        clear(&g);
        type(t, "help\r", 3);
        CHECK(!has(&g, "DELUSER"));
    }
}

static void onward_session(void)
{
    FakeLine f;
    ConsoleLine l = make_line(&f, 0);
    ConsoleSession *s;
    static const uint8_t esc = 0x1B;
    static const uint8_t del = 0x7F;
    setup(CONSOLE_MODE_PROMPT);
    s = login(&f, &l, "alice", "pw");

    type(s, "connect d102\r", 10);
    CHECK_EQ(s_node.opens, 1);
    CHECK(strcmp(s_node.system, "D102") == 0);
    CHECK_EQ(s_node.number, 102); /* the target's own number, from resolve */
    CHECK(strcmp(s_node.tad_user, "TELNET") == 0);
    CHECK(has(&f, "connecting to D102..."));
    clear(&f);
    type(s, "ab", 11); /* held until RUNNING */
    CHECK_EQ(s_node.typed_len, 0);
    CHECK(strcmp(f.out, "ab") == 0); /* echoed */
    console_onward_state(s, CONSOLE_ONWARD_RUNNING, 0);
    CHECK(s_node.typed_len == 2 && memcmp(s_node.typed, "ab", 2) == 0);
    clear(&f);
    type(s, "\r", 12);
    console_input(s, &del, 1, 12);
    CHECK(strcmp(f.out, "\b \b") == 0); /* no echo of CR; DEL takes the character back */

    console_onward_text(s, (const uint8_t *)"[HOST]\r\n", 8);
    CHECK(has(&f, "[HOST]")); /* the host's bytes untouched */

    /* Lone ESC: an escape after the wait. */
    console_input(s, &esc, 1, 20);
    console_host_poll(&s_host, 30);
    CHECK_EQ(s_node.escapes, 0);
    console_host_poll(&s_host, 80);
    CHECK_EQ(s_node.escapes, 1);

    /* ESC [ that never ends: held, then on as data after the sequence wait, not echoed. */
    clear(&f);
    s_node.typed_len = 0;
    type(s, "\x1b[", 100);
    CHECK_EQ(s_node.escapes, 1);
    CHECK_EQ(s_node.typed_len, 0);
    console_host_poll(&s_host, 299);
    CHECK_EQ(s_node.typed_len, 0);
    console_host_poll(&s_host, 300);
    CHECK(s_node.typed_len == 2 && s_node.typed[0] == 0x1B && s_node.typed[1] == '[');
    CHECK_EQ(f.len, 0);

    /* The TDV HELP key as measured live (2026-11-10 10:55:07): one write, one onward_write
     * call with all five bytes, nothing echoed. */
    {
        static const uint8_t help[] = {0x1B, 0x5B, 0x34, 0x36, 0x5F};
        s_node.typed_len = 0;
        s_node.writes = 0;
        console_input(s, help, sizeof help, 400);
        CHECK_EQ(s_node.writes, 1);
        CHECK(s_node.typed_len == 5 && memcmp(s_node.typed, help, 5) == 0);
        CHECK_EQ(f.len, 0);

        /* Split across reads with small gaps: still one call. */
        s_node.typed_len = 0;
        s_node.writes = 0;
        console_input(s, help, 1, 500);
        console_host_poll(&s_host, 520);
        console_input(s, &help[1], 2, 530);
        console_host_poll(&s_host, 560);
        CHECK_EQ(s_node.typed_len, 0);
        console_input(s, &help[3], 2, 600);
        CHECK_EQ(s_node.writes, 1);
        CHECK(s_node.typed_len == 5 && memcmp(s_node.typed, help, 5) == 0);
        CHECK_EQ(f.len, 0);

        /* Plain text around a key: the key goes in its own call; only the text is echoed. */
        s_node.typed_len = 0;
        s_node.writes = 0;
        console_input(s, (const uint8_t *)"a\x1b[46_b", 7, 700);
        CHECK_EQ(s_node.writes, 3);
        CHECK(s_node.typed_len == 7 && memcmp(s_node.typed, "a\x1b[46_b", 7) == 0);
        CHECK(f.len == 2 && memcmp(f.out, "ab", 2) == 0);
        clear(&f);

        /* The node cannot take it now: the key waits, then goes whole. */
        s_node.typed_len = 0;
        s_node.writes = 0;
        s_node.take = 0;
        console_input(s, help, sizeof help, 800);
        CHECK_EQ(s_node.typed_len, 0);
        s_node.take = -1;
        console_host_poll(&s_host, 810);
        CHECK_EQ(s_node.writes, 1);
        CHECK(s_node.typed_len == 5 && memcmp(s_node.typed, help, 5) == 0);
        CHECK_EQ(f.len, 0);
    }

    /* The host's echo wish. */
    clear(&f);
    console_onward_echo(s, false);
    type(s, "secret", 110);
    CHECK_EQ(f.len, 0);

    /* The writes the node cannot take wait in order. */
    s_node.typed_len = 0;
    s_node.take = 0;
    type(s, "xyz", 120);
    CHECK_EQ(s_node.typed_len, 0);
    s_node.take = -1;
    console_host_poll(&s_host, 130);
    CHECK(s_node.typed_len >= 3);

    /* SINTRAN logout: hang up; the end brings the prompt back (prompt mode). */
    console_onward_logout(s);
    CHECK_EQ(s_node.hangups, 1);
    console_onward_logout(s);
    CHECK_EQ(s_node.hangups, 1);
    clear(&f);
    console_onward_state(s, CONSOLE_ONWARD_CLOSED, 0);
    CHECK(strcmp(f.out, "\r\n<session closed>\r\nTAD> ") == 0);

    /* A plain number is a system number as it is; a name is never read as a number. */
    type(s, "connect 103\r", 150);
    CHECK_EQ(s_node.opens, 2);
    CHECK_EQ(s_node.number, 103);
    console_onward_state(s, CONSOLE_ONWARD_CLOSED, 0);
    clear(&f);
    type(s, "connect D103\r", 160);
    CHECK(has(&f, "unknown system D103"));
    CHECK_EQ(s_node.opens, 2);
    type(s, "connect 70000\r", 170);
    CHECK(has(&f, "unknown system 70000"));
    type(s, "connect 0\r", 170);
    CHECK(has(&f, "unknown system 0"));

    /* A failure says its error number. */
    type(s, "connect D100\r", 200);
    CHECK_EQ(s_node.number, 100);
    clear(&f);
    console_onward_state(s, CONSOLE_ONWARD_FAILED, 4);
    CHECK(has(&f, "<session failed, error 4>"));
}

static void direct_mode_and_line_lost(void)
{
    FakeLine f;
    ConsoleLine l = make_line(&f, 0);
    ConsoleSession *s;
    setup(CONSOLE_MODE_DIRECT);

    s = console_host_open(&s_host, &l, 0);
    CHECK(s != NULL);
    CHECK_EQ(s_node.opens, 1);
    CHECK(strcmp(s_node.system, "D100") == 0 && s_node.number == 100);
    CHECK(!has(&f, "login"));
    console_onward_state(s, CONSOLE_ONWARD_RUNNING, 0);
    console_onward_logout(s);
    CHECK_EQ(s_node.hangups, 1);
    console_onward_state(s, CONSOLE_ONWARD_CLOSED, 0);
    CHECK_EQ(f.closed, CONSOLE_CLOSE_SESSION_ENDED); /* direct: the line drops */
    CHECK(console_host_place_is_free(&s_host, 0));

    /* The line goes while the session runs: hang up; the place stays taken until the end. */
    s = console_host_open(&s_host, &l, 0);
    console_onward_state(s, CONSOLE_ONWARD_RUNNING, 0);
    console_line_lost(s);
    CHECK_EQ(s_node.hangups, 2);
    CHECK(!console_host_place_is_free(&s_host, 0));
    console_onward_state(s, CONSOLE_ONWARD_CLOSED, 0);
    CHECK(console_host_place_is_free(&s_host, 0));

    /* Full: every place taken. */
    CHECK(console_host_open(&s_host, &l, 0) != NULL);
    CHECK(console_host_open(&s_host, &l, 0) != NULL);
    CHECK(console_host_open(&s_host, &l, 0) != NULL);
    CHECK(console_host_open(&s_host, &l, 0) == NULL);
}

static bool clean_7bit(const FakeLine *f)
{
    for (size_t i = 0; i < f->len; i++)
    {
        if (strchr("[]{}|\\", f->out[i]) != NULL)
        {
            return false;
        }
    }
    return true;
}

static void seven_bit_own_echo_and_esca(void)
{
    FakeLine f;
    ConsoleLine l =
        make_line(&f, CONSOLE_LINE_7BIT | CONSOLE_LINE_OWN_ECHO | CONSOLE_LINE_ESCAPE_MSG);
    ConsoleSession *s;
    static const uint8_t esc = 0x1B;

    CHECK_EQ(console_7bit('['), '<');
    CHECK_EQ(console_7bit('{'), '<');
    CHECK_EQ(console_7bit(']'), '>');
    CHECK_EQ(console_7bit('}'), '>');
    CHECK_EQ(console_7bit('|'), '!');
    CHECK_EQ(console_7bit('\\'), '/');
    CHECK_EQ(console_7bit('A'), 'A');

    setup(CONSOLE_MODE_PROMPT);
    s = console_host_open(&s_host, &l, 0);
    CHECK(f.echo_calls == 1 && f.echo_last); /* the far side echoes the name */
    type(s, "alice\r", 1);
    CHECK(!f.echo_last);      /* ... and not the password */
    CHECK(!has(&f, "alice")); /* we never echo on such a line */
    type(s, "pw\r", 2);
    CHECK(f.echo_last);
    type(s, "help\r", 3);
    CHECK(has(&f, "ADDUSER name <admin>"));
    CHECK(clean_7bit(&f));

    type(s, "connect D102\r", 4);
    console_onward_state(s, CONSOLE_ONWARD_RUNNING, 0);
    s_node.typed_len = 0;
    console_input(s, &esc, 1, 5); /* ESC is text on an ESCA line */
    console_host_poll(&s_host, 500);
    CHECK_EQ(s_node.escapes, 0);
    CHECK(s_node.typed_len == 1 && s_node.typed[0] == 0x1B);
    console_escape(s);
    CHECK_EQ(s_node.escapes, 1);
    console_onward_echo(s, false);
    CHECK(!f.echo_last); /* the onward password: the caller's machine stops echoing */
    clear(&f);
    console_onward_text(s, (const uint8_t *)"[x]", 3);
    CHECK(has(&f, "[x]")); /* host text is not filtered */
    clear(&f);
    console_onward_state(s, CONSOLE_ONWARD_CLOSED, 0);
    CHECK(clean_7bit(&f) && has(&f, "<session closed>"));
}

static void trusted_admin_and_names(void)
{
    FakeLine f;
    ConsoleLine l = make_line(&f, CONSOLE_LINE_TRUSTED_ADMIN);
    ConsoleSession *s;
    ConsoleConfig cfg;
    char name[CONSOLE_TAD_USER_MAX + 1u];
    setup(CONSOLE_MODE_PROMPT);

    s = console_host_open(&s_host, &l, 0);
    CHECK(strcmp(f.out, "TAD> ") == 0);
    CHECK_EQ(f.logins, 1);
    type(s, "users\r", 1);
    CHECK(has(&f, "alice"));
    type(s, "password\r", 1);
    CHECK(has(&f, "did not log in with a password"));

    console_config_default(&cfg);
    CHECK(console_tad_user_name(&cfg, "alice", name, sizeof name) == NULL);
    cfg.tad_user = "TELNET";
    CHECK(strcmp(console_tad_user_name(&cfg, "alice", name, sizeof name), "TELNET") == 0);
    cfg.tad_user_suffix = "xmsggw";
    CHECK(strcmp(console_tad_user_name(&cfg, "alice", name, sizeof name), "alice@xmsggw") == 0);
    CHECK(strcmp(console_tad_user_name(&cfg, "abcdefghij", name, sizeof name), "abcdefghij") ==
          0); /* "abcdefghij@xmsggw" is 17 */
    CHECK(strcmp(console_tad_user_name(&cfg, "abcdefghijklmnopqrs", name, sizeof name),
                 "abcdefghijklmnop") == 0);
    CHECK(strcmp(console_tad_user_name(&cfg, NULL, name, sizeof name), "TELNET") == 0);
    CHECK_EQ(console_host_init(&s_host, &cfg, s_sessions, 3), -1); /* no ports */
}

/* An application command hook, as XMSGGW's NODE and RESTART use it. */
static int s_app_calls;

static int fn_command(void *ctx, int place, const char *line, AuthLevel level, char *out,
                      size_t cap)
{
    (void)ctx;
    s_app_calls++;
    if (strcmp(line, "node") != 0)
    {
        return 0;
    }
    (void)snprintf(out, cap, "node %d %s\r\n", place,
                   (level == AUTH_LEVEL_ADMIN) ? "admin" : "user");
    return 1;
}

/* An FTP user that is not on the console, as XMSGGW shows it in WHO. */
static int fn_who_more(void *ctx, char *buf, size_t cap)
{
    (void)ctx;
    return snprintf(buf, cap, "  ftp  bob  ftp 10.0.0.9:51000\r\n");
}

static void application_commands(void)
{
    FakeLine f;
    ConsoleLine l = make_line(&f, 0);
    ConsoleSession *s;
    setup(CONSOLE_MODE_PROMPT);
    s_app_calls = 0;
    s_host.cfg.services.command = fn_command;
    s_host.cfg.services.help_user = "NODE               the node (app)\r\n";
    s_host.cfg.services.help_admin = "RESTART            restart (app admin)\r\n";
    s_host.cfg.services.who_more = fn_who_more;
    s = login(&f, &l, "alice", "pw");
    type(s, "node\r", 3);
    CHECK(has(&f, "node 0 admin\r\nTAD> "));
    clear(&f);
    type(s, "frobnicate\r", 3);
    CHECK(has(&f, "unknown command"));
    CHECK_EQ(s_app_calls, 2);
    clear(&f);
    type(s, "who\r", 3); /* a core command never reaches the application */
    CHECK_EQ(s_app_calls, 2);
    /* The application's own WHO lines come after the console sessions. */
    CHECK(has(&f, "*    1 alice") && has(&f, "ftp  bob  ftp 10.0.0.9:51000\r\nTAD> "));
    clear(&f);
    type(s, "help\r", 3);
    CHECK(has(&f, "NODE               the node (app)") && has(&f, "RESTART            restart"));
    {
        FakeLine g;
        ConsoleLine m = make_line(&g, 0);
        ConsoleSession *t = login(&g, &m, "carol", "cpw");
        type(t, "help\r", 3);
        CHECK(has(&g, "NODE") && !has(&g, "RESTART"));
        clear(&g);
        type(t, "node\r", 3);
        CHECK(has(&g, "node 1 user"));
    }
}

/* An application command whose answer comes later (XMSGGW LIST-FILES over XMSG). */
static int s_cancels;
static int s_cancel_place;

static int fn_command_later(void *ctx, int place, const char *line, AuthLevel level, char *out,
                            size_t cap)
{
    (void)ctx;
    (void)level;
    (void)place;
    if (strcmp(line, "list-files d100(system)") != 0)
    {
        return 0;
    }
    (void)snprintf(out, cap, "asking [D100]\r\n");
    return 2;
}

static void fn_command_cancel(void *ctx, int place)
{
    (void)ctx;
    s_cancels++;
    s_cancel_place = place;
}

static void application_command_answered_later(void)
{
    FakeLine f;
    ConsoleLine l = make_line(&f, 0);
    ConsoleSession *s;
    uint8_t esc = 0x1Bu;
    setup(CONSOLE_MODE_PROMPT);
    s_cancels = 0;
    s_host.cfg.services.command = fn_command_later;
    s_host.cfg.services.command_cancel = fn_command_cancel;
    s = login(&f, &l, "alice", "pw");
    clear(&f);
    type(s, "list-files d100(system)\r", 3);
    CHECK(has(&f, "asking [D100]\r\n"));
    CHECK(!has(&f, "TAD> "));
    CHECK_EQ(console_command_text(s, "  CHATLG2:SYMB  255\r\n"), 21);
    type(s, "who\r", 3);
    CHECK(has(&f, "still running"));
    clear(&f);
    console_command_done(s);
    CHECK(has(&f, "TAD> "));
    CHECK_EQ(console_command_text(s, "late\r\n"), -1); /* nothing running */
    CHECK_EQ(s_cancels, 0);

    /* The escape key as a byte stops it. */
    type(s, "list-files d100(system)\r", 3);
    clear(&f);
    console_input(s, &esc, 1, 4);
    CHECK_EQ(s_cancels, 1);
    CHECK(has(&f, "<cancelled>\r\nTAD> "));
    CHECK_EQ(console_command_text(s, "x\r\n"), -1);

    /* As a message (TAD ESCA), and the user going away. */
    type(s, "list-files d100(system)\r", 3);
    console_escape(s);
    CHECK_EQ(s_cancels, 2);
    type(s, "list-files d100(system)\r", 3);
    console_line_lost(s);
    CHECK_EQ(s_cancels, 3);
    CHECK_EQ(s_cancel_place, 0);

    /* On a 7-bit line: our own text gets the 7-bit rule, a file's bytes (COPY-TERM) do not. */
    {
        FakeLine g;
        ConsoleLine m = make_line(&g, CONSOLE_LINE_7BIT);
        ConsoleSession *t = login(&g, &m, "carol", "cpw");
        type(t, "list-files d100(system)\r", 3);
        clear(&g);
        CHECK_EQ(console_command_text(t, "[a]\r\n"), 5);
        CHECK(has(&g, "<a>\r\n"));
        CHECK_EQ(console_command_raw(t, "{b|c}\r\n"), 7);
        CHECK(has(&g, "{b|c}\r\n"));
        console_command_done(t);
        CHECK_EQ(console_command_raw(t, "x"), -1);
    }
}

/* NAMES <system> asks that system; CONNECT and NAMES to our own system (19999 here) are refused
 * (XMSGGW, 2026-11-10: CONNECT 101 on node 101 hung). */
static void names_of_a_system_and_own_system_refused(void)
{
    FakeLine f;
    ConsoleLine l = make_line(&f, 0);
    ConsoleSession *s;
    setup(CONSOLE_MODE_PROMPT);
    s = login(&f, &l, "alice", "pw");
    type(s, "names\r", 3);
    CHECK_EQ(s_node.names, 1);
    CHECK_EQ(s_node.names_system, 0);
    console_names_done(s, XMSG_OK, 0);
    clear(&f);
    type(s, "names D102\r", 3);
    CHECK_EQ(s_node.names, 2);
    CHECK_EQ(s_node.names_system, 102);
    CHECK(has(&f, "Names known to node 102:"));
    console_names_done(s, XMSG_OK, 0);
    clear(&f);
    type(s, "names 103\r", 3);
    CHECK_EQ(s_node.names_system, 103);
    console_names_done(s, XMSG_OK, 0);
    clear(&f);
    type(s, "names D999\r", 3);
    CHECK(has(&f, "unknown system D999"));
    CHECK_EQ(s_node.names, 3);
    clear(&f);
    type(s, "names 19999\r", 3);
    CHECK(has(&f, "19999 is this gateway; it has no host of its own to ask\r\nTAD> "));
    CHECK_EQ(s_node.names, 3);
    clear(&f);
    type(s, "connect 19999\r", 3);
    CHECK(has(&f, "19999 is this gateway; you cannot connect to the gateway you are on\r\nTAD> "));
    CHECK_EQ(s_node.opens, 0);
}

int main(void)
{
    printf("[console]\n");
    login_ok_and_bad();
    must_change_and_restore();
    commands();
    onward_session();
    direct_mode_and_line_lost();
    seven_bit_own_echo_and_esca();
    trusted_admin_and_names();
    application_commands();
    application_command_answered_later();
    names_of_a_system_and_own_system_refused();
    TEST_DONE();
}
