/*
 * test_console_tad.c - a SINTRAN caller on the console, end to end over two real nodes.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Node 100 plays the SINTRAN machine: our own tad_asker types CONNECT-TO. Node 102 carries a
 * ConsoleTadLine (a TadResponder bridged to the console) and a ConsoleHost. The asker's user
 * logs in, runs WHO, and logs out with EXIT; the responder hangs up and both ends close.
 */
#include "libxmsg/console/console.h"
#include "libxmsg/console/console_tad.h"
#include "libxmsg/tad/tad_asker.h"
#include "libxmsg/xmsg/xmsg_node.h"
#include "libxmsg/xmsg_errors.h"

#include "pair/test_pair.h"
#include "test_util.h"

typedef struct Store
{
    uint16_t next;
    uint16_t peer;
} Store;

static int f1_load(void *ctx, uint16_t node, uint16_t *next)
{
    Store *st = (Store *)ctx;
    if (node != st->peer)
    {
        return -1;
    }
    *next = st->next;
    return 0;
}

static int f1_save(void *ctx, uint16_t node, uint16_t next)
{
    Store *st = (Store *)ctx;
    if (node == st->peer)
    {
        st->next = next;
    }
    return 0;
}

static XmsgNode s_a; /* 100: the SINTRAN caller's machine */
static XmsgNode s_b; /* 102: ours */
static TestPair s_pair;
static TestPairPort s_port_a;
static TestPairPort s_port_b;
static Store s_fa;
static Store s_fb;
static ConsoleTadLine s_line;
static ConsoleHost s_host;
static ConsoleSession s_sessions[2];
static TadAsker s_asker;
static AuthUser s_user;

/* ------------------------------------------------------------------ the console's ports */

static int u_lookup(void *ctx, const char *name, AuthUser *out)
{
    (void)ctx;
    if (strcmp(name, s_user.name) != 0)
    {
        return -1;
    }
    *out = s_user;
    return 0;
}

static int rng_fill(void *ctx, uint8_t *buf, size_t len)
{
    (void)ctx;
    memset(buf, 0x5A, len);
    return 0;
}

static int sv_resolve(void *ctx, const char *system, uint16_t *number)
{
    (void)ctx;
    (void)system;
    (void)number;
    return -1;
}

static int sv_open(void *ctx, int place, const char *system, uint16_t number,
                   const char *user)
{
    (void)ctx;
    (void)place;
    (void)system;
    (void)number;
    (void)user;
    return XMSG_ENOSPC;
}

static int sv_write(void *ctx, int place, const uint8_t *b, size_t n)
{
    (void)ctx;
    (void)place;
    (void)b;
    return (int)n;
}

static void sv_hangup(void *ctx, int place)
{
    (void)ctx;
    (void)place;
}

static void tad_status(void *ctx, uint16_t *in_use, uint16_t *total)
{
    (void)ctx;
    console_host_counts(&s_host, in_use, total);
}

/* ------------------------------------------------------------------ the caller */

typedef struct Far
{
    char text[4096];
    size_t len;
    uint8_t eckm[16];
    size_t eckm_n;
} Far;

static Far s_far;

static void f_text(void *ctx, const uint8_t *b, size_t n)
{
    Far *f = (Far *)ctx;
    if (f->len + n < sizeof f->text)
    {
        memcpy(f->text + f->len, b, n);
        f->len += n;
        f->text[f->len] = '\0';
    }
}

static void f_echo(void *ctx, uint8_t e)
{
    Far *f = (Far *)ctx;
    if (f->eckm_n < sizeof f->eckm)
    {
        f->eckm[f->eckm_n++] = e;
    }
}

/* ------------------------------------------------------------------ the loop */

static void b_on_letter(void *ctx, const XmsgDatagram *dg, uint32_t now_ms)
{
    (void)ctx;
    (void)console_tad_offer(&s_line, &s_b, dg, now_ms);
}

/* One pass of both machines, as the host tool's loop does it. */
static void pass(uint32_t now)
{
    uint8_t chunk[256];
    size_t n;
    if (console_tad_needs_session(&s_line))
    {
        ConsoleLine l;
        console_tad_line(&s_line, &l);
        console_tad_attach(&s_line, console_host_open(&s_host, &l, now));
    }
    (void)console_tad_deliver(&s_line, now);
    (void)console_tad_release(&s_line);
    console_host_poll(&s_host, now);
    console_tad_pump(&s_line, now);
    xmsg_node_tick(&s_a, now);
    xmsg_node_tick(&s_b, now);
    while ((n = test_pair_read(&s_pair, 0, chunk, sizeof chunk)) > 0)
    {
        xmsg_node_feed(&s_a, chunk, n, now);
    }
    while ((n = test_pair_read(&s_pair, 1, chunk, sizeof chunk)) > 0)
    {
        xmsg_node_feed(&s_b, chunk, n, now);
    }
}

static uint32_t run(uint32_t now, int ms)
{
    for (int i = 0; i < ms; i++)
    {
        now++;
        pass(now);
    }
    return now;
}

/* Run until the caller's text holds `want` (or a time limit). */
static uint32_t until(uint32_t now, const char *want)
{
    for (int i = 0; i < 3000 && strstr(s_far.text, want) == NULL; i++)
    {
        now = run(now, 1);
    }
    CHECK(strstr(s_far.text, want) != NULL);
    return now;
}

/* Type a line once the caller may type. */
static uint32_t type(uint32_t now, const char *text)
{
    for (int i = 0; i < 3000 && !tad_asker_may_type(&s_asker); i++)
    {
        now = run(now, 1);
    }
    CHECK(tad_asker_may_type(&s_asker));
    CHECK_EQ(tad_asker_write(&s_asker, (const uint8_t *)text, strlen(text), now),
             (int)strlen(text));
    return now;
}

static uint32_t bring_up(void)
{
    XmsgTransportPort ta;
    XmsgTransportPort tb;
    XmsgNodeConfig cfg;
    XmsgPorts ports;
    XmsgNodeCallbacks cb;
    uint32_t now = 0;

    s_fa.peer = 102;
    s_fb.peer = 100;
    test_pair_init(&s_pair);
    test_pair_transport(&s_pair, 0, &s_port_a, &ta);
    test_pair_transport(&s_pair, 1, &s_port_b, &tb);

    xmsg_node_config_default(&cfg, 100);
    cfg.lapb.t1_ms = 50;
    cfg.settle_ms = 20;
    memset(&ports, 0, sizeof ports);
    ports.transport = ta;
    ports.flags1.ctx = &s_fa;
    ports.flags1.load = f1_load;
    ports.flags1.save = f1_save;
    memset(&cb, 0, sizeof cb);
    CHECK_EQ(xmsg_node_init(&s_a, &cfg, &ports, &cb), XMSG_OK);

    xmsg_node_config_default(&cfg, 102);
    cfg.lapb.t1_ms = 50;
    cfg.settle_ms = 20;
    memset(&ports, 0, sizeof ports);
    ports.transport = tb;
    ports.flags1.ctx = &s_fb;
    ports.flags1.load = f1_load;
    ports.flags1.save = f1_save;
    memset(&cb, 0, sizeof cb);
    cb.on_letter = b_on_letter;
    CHECK_EQ(xmsg_node_init(&s_b, &cfg, &ports, &cb), XMSG_OK);

    xmsg_node_link_start(&s_a, now);
    xmsg_node_link_start(&s_b, now);
    while (!(xmsg_node_is_ready(&s_a) && xmsg_node_is_ready(&s_b)) && now < 5000)
    {
        now++;
        pass(now);
    }
    CHECK(xmsg_node_is_ready(&s_a) && xmsg_node_is_ready(&s_b));
    return now;
}

static void setup_console(void)
{
    ConsoleConfig cfg;
    ConsoleTadConfig tc;
    AuthRandom rng = {NULL, rng_fill};
    CHECK_EQ(auth_user_make(&s_user, "ronny", "pw", 2u, &rng), 0);
    console_config_default(&cfg);
    cfg.banner = "libxmsg test bridge\r\n";
    cfg.iterations = 2u;
    cfg.store.lookup = u_lookup;
    cfg.rng.fill = rng_fill;
    cfg.services.resolve = sv_resolve;
    cfg.services.onward_open = sv_open;
    cfg.services.onward_write = sv_write;
    cfg.services.onward_hangup = sv_hangup;
    CHECK_EQ(console_host_init(&s_host, &cfg, s_sessions, 2), 0);
    console_tad_config_default(&tc);
    tc.status = tad_status;
    CHECK_EQ(console_tad_init(&s_line, &tc), 0);
}

static void caller_logs_in_runs_who_and_exits(void)
{
    TadAskerConfig cfg;
    TadAskerCallbacks cb = {&s_far, f_text, NULL, NULL, f_echo, NULL, NULL};
    uint32_t now;

    setup_console();
    now = bring_up();
    memset(&s_far, 0, sizeof s_far);
    tad_asker_config_default(&cfg, 102, "D102");
    CHECK_EQ(tad_asker_init(&s_asker, &cfg, &cb), XMSG_OK);
    CHECK_EQ(tad_asker_connect(&s_asker, &s_a, now), XMSG_OK);

    now = until(now, "login: ");
    CHECK(strstr(s_far.text, "libxmsg test bridge\r\n") != NULL);
    CHECK(s_far.eckm_n >= 1 && s_far.eckm[0] == 0x01); /* the caller's machine echoes */
    CHECK(console_tad_state(&s_line) == CONSOLE_TAD_RUNNING);
    CHECK(s_line.session != NULL);

    now = type(now, "ronny\r");
    now = until(now, "password: ");
    CHECK_EQ(s_far.eckm[s_far.eckm_n - 1], 0xFF); /* no echo for the password */
    CHECK(strstr(s_far.text, "ronny") == NULL);   /* the console never echoes on this line */

    now = type(now, "pw\r");
    now = until(now, "TAD> ");
    CHECK(strstr(s_far.text, "Logged in.") != NULL);
    CHECK_EQ(s_far.eckm[s_far.eckm_n - 1], 0x01);
    CHECK_EQ(s_line.r.stats.text_bytes_in, 9);

    s_far.len = 0;
    s_far.text[0] = '\0';
    now = type(now, "who\r");
    now = until(now, "TAD> ");
    CHECK(strstr(s_far.text, "*    1 ronny") != NULL);
    CHECK(strstr(s_far.text, "TAD system 100 port 0x") != NULL);
    CHECK(strstr(s_far.text, "at the prompt") != NULL);

    s_far.len = 0;
    s_far.text[0] = '\0';
    now = type(now, "help\r");
    now = until(now, "TAD> ");
    CHECK(strstr(s_far.text, "ADDUSER name <admin>") == NULL); /* not admin: no admin lines */
    for (size_t i = 0; i < s_far.len; i++)
    {
        CHECK(strchr("[]{}|\\", s_far.text[i]) == NULL); /* 7-bit text */
    }

    now = type(now, "exit\r");
    now = until(now, "bye\r\n");
    for (int i = 0; i < 3000 && tad_asker_state(&s_asker) != TAD_ASKER_CLOSED; i++)
    {
        now = run(now, 1);
    }
    CHECK_EQ(tad_asker_state(&s_asker), TAD_ASKER_CLOSED);
    now = run(now, 500);
    CHECK_EQ(console_tad_state(&s_line), CONSOLE_TAD_IDLE); /* released for the next call */
    CHECK(console_host_place_is_free(&s_host, 0));
    CHECK(console_host_place_is_free(&s_host, 1)); /* no second session was opened */
    CHECK_EQ(s_a.stats.bad_headers + s_b.stats.bad_headers, 0);
}

int main(void)
{
    printf("[console_tad]\n");
    caller_logs_in_runs_who_and_exits();
    TEST_DONE();
}
