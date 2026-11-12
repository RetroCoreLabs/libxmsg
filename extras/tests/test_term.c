/*
 * test_term.c - the lone-ESC detector, the function key gatherer, the line editor, the command words, WHO/INFO text and
 * the telnet codec.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/term/term_escape.h"
#include "libxmsg/term/term_keyseq.h"
#include "libxmsg/term/term_line.h"
#include "libxmsg/term/term_report.h"
#include "libxmsg/term/term_telnet.h"
#include "test_util.h"

/* Feed bytes at given times; collect the data passed on and count escapes, in order. Escapes are
 * written into the output as the marker 0x100 so their place in the stream is checked too. */
typedef struct Run
{
    int out[64];
    size_t n;
} Run;

static void feed(TermEscape *e, Run *r, uint8_t b, uint32_t now)
{
    uint8_t o[2];
    bool esc = false;
    int k = term_escape_byte(e, b, now, o, &esc);
    CHECK(k >= 0 && k <= 2);
    if (esc)
    {
        r->out[r->n++] = 0x100;
    }
    for (int i = 0; i < k; i++)
    {
        r->out[r->n++] = o[i];
    }
}

static void poll(TermEscape *e, Run *r, uint32_t now)
{
    if (term_escape_poll(e, now))
    {
        r->out[r->n++] = 0x100;
    }
}

static void lone_esc_is_an_escape(void)
{
    TermEscape e;
    Run r = {{0}, 0};
    term_escape_init(&e, 50);
    feed(&e, &r, 0x1B, 1000);
    CHECK_EQ(r.n, 0); /* held */
    poll(&e, &r, 1049);
    CHECK_EQ(r.n, 0); /* not yet */
    poll(&e, &r, 1050);
    CHECK_EQ(r.n, 1);
    CHECK_EQ(r.out[0], 0x100);
    poll(&e, &r, 2000);
    CHECK_EQ(r.n, 1); /* used up */
    feed(&e, &r, 'a', 2000);
    CHECK_EQ(r.n, 2);
    CHECK_EQ(r.out[1], 'a');
}

static void function_key_passes_intact(void)
{
    /* The TDV function key Ronny tests with: ESC [ 4 6 _ in one burst. */
    static const uint8_t k_key[] = {0x1B, '[', '4', '6', '_'};
    TermEscape e;
    Run r = {{0}, 0};
    term_escape_init(&e, 50);
    for (size_t i = 0; i < sizeof k_key; i++)
    {
        feed(&e, &r, k_key[i], 3000);
    }
    poll(&e, &r, 4000);
    CHECK_EQ(r.n, sizeof k_key);
    for (size_t i = 0; i < sizeof k_key && i < r.n; i++)
    {
        CHECK_EQ(r.out[i], k_key[i]);
    }
}

static void esc_at_end_of_a_read_then_byte_soon(void)
{
    /* ESC is the last byte of one TCP read; '[' arrives 10 ms later in the next read. */
    TermEscape e;
    Run r = {{0}, 0};
    term_escape_init(&e, 50);
    feed(&e, &r, 0x1B, 500);
    poll(&e, &r, 505);
    feed(&e, &r, '[', 510);
    CHECK_EQ(r.n, 2);
    CHECK_EQ(r.out[0], 0x1B);
    CHECK_EQ(r.out[1], '[');
}

static void late_byte_after_an_unpolled_esc(void)
{
    /* Nobody polled, the next key came 200 ms later: the ESC was the key, then the byte. */
    TermEscape e;
    Run r = {{0}, 0};
    term_escape_init(&e, 50);
    feed(&e, &r, 0x1B, 0);
    feed(&e, &r, 'x', 200);
    CHECK_EQ(r.n, 2);
    CHECK_EQ(r.out[0], 0x100);
    CHECK_EQ(r.out[1], 'x');
}

static void two_esc_in_a_row(void)
{
    TermEscape e;
    Run r = {{0}, 0};
    term_escape_init(&e, 50);
    feed(&e, &r, 0x1B, 0);
    feed(&e, &r, 0x1B, 5); /* first passes as data, second held */
    CHECK_EQ(r.n, 1);
    CHECK_EQ(r.out[0], 0x1B);
    poll(&e, &r, 60);
    CHECK_EQ(r.n, 2);
    CHECK_EQ(r.out[1], 0x100);
}

static void zero_means_always_data(void)
{
    TermEscape e;
    Run r = {{0}, 0};
    term_escape_init(&e, 0);
    feed(&e, &r, 0x1B, 0);
    poll(&e, &r, 100000);
    CHECK_EQ(r.n, 1);
    CHECK_EQ(r.out[0], 0x1B);
    CHECK_EQ(term_escape_byte(NULL, 0, 0, NULL, NULL), -1);
}

/* ------------------------------------------------------------------ function keys */

/* What the key gatherer gave, in order: 0x100 = escape, 0x200 + n = the start of a unit of n
 * bytes (its bytes follow), 0x300 = a plain byte (it follows). */
typedef struct KRun
{
    int out[128];
    size_t n;
    unsigned whole; /* units that ended on their final byte */
} KRun;

static void k_take(KRun *r, const TermKeyOut *o)
{
    if (o->escape)
    {
        r->out[r->n++] = 0x100;
    }
    if (o->unit_len != 0u)
    {
        r->out[r->n++] = 0x200 + (int)o->unit_len;
        for (size_t i = 0; i < o->unit_len; i++)
        {
            r->out[r->n++] = o->unit[i];
        }
        if (o->whole)
        {
            r->whole++;
        }
    }
    if (o->has_byte)
    {
        r->out[r->n++] = 0x300;
        r->out[r->n++] = o->byte;
    }
}

static void k_feed(TermKeyseq *k, KRun *r, const uint8_t *b, size_t n, uint32_t now)
{
    for (size_t i = 0; i < n; i++)
    {
        TermKeyOut o;
        CHECK_EQ(term_keyseq_byte(k, b[i], now, &o), 0);
        k_take(r, &o);
    }
}

static void k_poll(TermKeyseq *k, KRun *r, uint32_t now)
{
    TermKeyOut o;
    if (term_keyseq_poll(k, now, &o))
    {
        k_take(r, &o);
    }
}

/* The bytes measured live 2026-11-10 10:55:07: the TDV HELP key, ESC [ 4 6 _. */
static const uint8_t k_help[] = {0x1B, 0x5B, 0x34, 0x36, 0x5F};

static void k_is_help_unit(const KRun *r, size_t at)
{
    CHECK(r->n >= at + 6u);
    CHECK_EQ(r->out[at], 0x205);
    for (size_t i = 0; i < sizeof k_help && at + 1u + i < r->n; i++)
    {
        CHECK_EQ(r->out[at + 1u + i], k_help[i]);
    }
}

static void keyseq_help_key_in_one_chunk(void)
{
    TermKeyseq k;
    KRun r = {{0}, 0, 0};
    term_keyseq_init(&k, TERM_KEYSEQ_ESC_MS, TERM_KEYSEQ_SEQ_MS);
    k_feed(&k, &r, k_help, 4, 1000);
    CHECK_EQ(r.n, 0); /* ESC [ 4 6: held, no final byte yet */
    CHECK(term_keyseq_holding(&k));
    k_feed(&k, &r, &k_help[4], 1, 1000);
    CHECK_EQ(r.n, 6); /* one unit of five, nothing else */
    k_is_help_unit(&r, 0);
    CHECK_EQ(r.whole, 1);
    CHECK(!term_keyseq_holding(&k));
    k_poll(&k, &r, 5000);
    CHECK_EQ(r.n, 6); /* nothing left over */
}

static void keyseq_help_key_split_with_small_gaps(void)
{
    /* As TCP may cut it: ESC | [ 4 | 6 | _, polled in between. */
    TermKeyseq k;
    KRun r = {{0}, 0, 0};
    term_keyseq_init(&k, TERM_KEYSEQ_ESC_MS, TERM_KEYSEQ_SEQ_MS);
    k_feed(&k, &r, &k_help[0], 1, 100);
    k_poll(&k, &r, 120);
    k_feed(&k, &r, &k_help[1], 2, 130); /* 30 ms after the ESC: inside the lone-ESC wait */
    k_poll(&k, &r, 200);
    k_feed(&k, &r, &k_help[3], 1, 250);
    k_poll(&k, &r, 300);
    CHECK_EQ(r.n, 0);
    k_feed(&k, &r, &k_help[4], 1, 400); /* 150 ms gap: still inside the sequence wait */
    CHECK_EQ(r.n, 6);
    k_is_help_unit(&r, 0);
    CHECK_EQ(r.whole, 1);
}

static void keyseq_other_shapes(void)
{
    static const uint8_t up[] = {0x1B, '[', 'A'};        /* VT100 cursor up */
    static const uint8_t page[] = {0x1B, '[', '5', '~'}; /* VT220 prev screen */
    static const uint8_t pf1[] = {0x1B, 'O', 'P'};       /* SS3 PF1 */
    static const uint8_t two[] = {0x1B, '='};            /* ESC + one byte */
    static const uint8_t inter[] = {0x1B, ' ', 'F'};     /* intermediate, then final */
    TermKeyseq k;
    KRun r = {{0}, 0, 0};
    term_keyseq_init(&k, TERM_KEYSEQ_ESC_MS, TERM_KEYSEQ_SEQ_MS);
    k_feed(&k, &r, up, sizeof up, 10);
    k_feed(&k, &r, page, sizeof page, 20);
    k_feed(&k, &r, pf1, sizeof pf1, 30);
    k_feed(&k, &r, two, sizeof two, 40);
    k_feed(&k, &r, inter, sizeof inter, 50);
    CHECK_EQ(r.whole, 5);
    CHECK_EQ(r.n, 4u + 5u + 4u + 3u + 4u);
    CHECK_EQ(r.out[0], 0x203);
    CHECK_EQ(r.out[4], 0x204);
    CHECK_EQ(r.out[9], 0x203);
    CHECK_EQ(r.out[13], 0x202);
    CHECK_EQ(r.out[16], 0x203);
}

static void keyseq_unterminated(void)
{
    TermKeyseq k;
    KRun r = {{0}, 0, 0};
    uint8_t many[TERM_KEYSEQ_MAX + 2u];
    static const uint8_t cut[] = {0x1B, '[', '4', '\r'};
    term_keyseq_init(&k, TERM_KEYSEQ_ESC_MS, TERM_KEYSEQ_SEQ_MS);

    /* It stops coming: after the sequence wait it goes on as data, every byte. */
    k_feed(&k, &r, k_help, 3, 0);
    k_poll(&k, &r, 199);
    CHECK_EQ(r.n, 0);
    k_poll(&k, &r, 200);
    CHECK_EQ(r.n, 4);
    CHECK_EQ(r.out[0], 0x203);
    CHECK_EQ(r.out[1], 0x1B);
    CHECK_EQ(r.out[3], '4');
    CHECK_EQ(r.whole, 0);

    /* Broken by a byte that cannot be in it: held bytes, then that byte on its own. */
    r.n = 0;
    k_feed(&k, &r, cut, sizeof cut, 1000);
    CHECK_EQ(r.n, 6);
    CHECK_EQ(r.out[0], 0x203);
    CHECK_EQ(r.out[4], 0x300);
    CHECK_EQ(r.out[5], '\r');

    /* Too long to be a key: out as data at the cap; the rest are plain bytes. */
    r.n = 0;
    many[0] = 0x1B;
    many[1] = '[';
    for (size_t i = 2; i < sizeof many; i++)
    {
        many[i] = '1';
    }
    k_feed(&k, &r, many, sizeof many, 2000);
    CHECK_EQ(r.out[0], 0x200 + (int)TERM_KEYSEQ_MAX);
    CHECK_EQ(r.n, 1u + TERM_KEYSEQ_MAX + 4u);
    CHECK_EQ(r.out[1u + TERM_KEYSEQ_MAX], 0x300);
    CHECK_EQ(r.whole, 0);
}

static void keyseq_esc_esc(void)
{
    static const uint8_t ee[] = {0x1B, 0x1B};
    TermKeyseq k;
    KRun r = {{0}, 0, 0};
    term_keyseq_init(&k, TERM_KEYSEQ_ESC_MS, TERM_KEYSEQ_SEQ_MS);
    k_feed(&k, &r, ee, sizeof ee, 0); /* the first goes on as data, the second is held */
    CHECK_EQ(r.n, 2);
    CHECK_EQ(r.out[0], 0x201);
    CHECK_EQ(r.out[1], 0x1B);
    k_poll(&k, &r, 50);
    CHECK_EQ(r.n, 3);
    CHECK_EQ(r.out[2], 0x100); /* ... and is the escape key when nothing follows */

    /* ESC ESC [ 4 6 _: a data ESC, then the whole key. */
    r.n = 0;
    k_feed(&k, &r, ee, 1, 1000);
    k_feed(&k, &r, k_help, sizeof k_help, 1000);
    CHECK_EQ(r.n, 2u + 6u);
    CHECK_EQ(r.out[0], 0x201);
    k_is_help_unit(&r, 2);
}

static void keyseq_lone_esc(void)
{
    TermKeyseq k;
    KRun r = {{0}, 0, 0};
    term_keyseq_init(&k, TERM_KEYSEQ_ESC_MS, TERM_KEYSEQ_SEQ_MS);
    k_feed(&k, &r, k_help, 1, 1000);
    k_poll(&k, &r, 1049);
    CHECK_EQ(r.n, 0);
    k_poll(&k, &r, 1050);
    CHECK_EQ(r.n, 1);
    CHECK_EQ(r.out[0], 0x100);
    k_poll(&k, &r, 3000);
    CHECK_EQ(r.n, 1); /* used up */

    /* Not polled, the next key came late: the escape, then that key as a plain byte. */
    r.n = 0;
    k_feed(&k, &r, k_help, 1, 5000);
    k_feed(&k, &r, (const uint8_t *)"x", 1, 5200);
    CHECK_EQ(r.n, 3);
    CHECK_EQ(r.out[0], 0x100);
    CHECK_EQ(r.out[1], 0x300);
    CHECK_EQ(r.out[2], 'x');

    /* esc_ms 0 (the escape key comes as its own message): a lone ESC is data, after the wait. */
    term_keyseq_init(&k, 0, TERM_KEYSEQ_SEQ_MS);
    r.n = 0;
    k_feed(&k, &r, k_help, 1, 0);
    k_poll(&k, &r, 100);
    CHECK_EQ(r.n, 0);
    k_poll(&k, &r, 200);
    CHECK_EQ(r.n, 2);
    CHECK_EQ(r.out[0], 0x201);
    CHECK_EQ(r.out[1], 0x1B);
    CHECK_EQ(term_keyseq_byte(NULL, 0, 0, NULL), -1);
    CHECK(!term_keyseq_poll(NULL, 0, NULL));
}

/* ------------------------------------------------------------------ line editor */

typedef struct Typed
{
    uint8_t echo[256];
    size_t echo_len;
    char lines[4][TERM_LINE_MAX + 1u];
    int nlines;
} Typed;

static void type(TermLine *l, Typed *t, const char *bytes, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        uint8_t e[3];
        size_t el = 0;
        int done = term_line_byte(l, (uint8_t)bytes[i], e, &el);
        memcpy(t->echo + t->echo_len, e, el);
        t->echo_len += el;
        if (done == 1 && t->nlines < 4)
        {
            memcpy(t->lines[t->nlines++], l->text, l->len + 1u);
            term_line_clear(l);
        }
    }
}

static void line_editing(void)
{
    TermLine l;
    Typed t;

    memset(&t, 0, sizeof t);
    term_line_init(&l, true);
    type(&l, &t, "conx\x08nect d102\r\n", 16);
    CHECK_EQ(t.nlines, 1);
    CHECK(strcmp(t.lines[0], "connect d102") == 0);
    /* "conx", BS SP BS, "nect d102", CR LF; the client's LF after CR is not echoed again. */
    CHECK_BYTES(t.echo, t.echo_len, "636F6E78 082008 6E6563742064313032 0D0A");

    /* CR NUL, bare CR, DEL, DEL on an empty line, a bare LF; control bytes ignored. */
    memset(&t, 0, sizeof t);
    term_line_init(&l, true);
    type(&l, &t, "ab\r\0", 4);
    type(&l, &t, "\x7F\x7Fx\x01y\r", 6);
    type(&l, &t, "who\n", 4);
    CHECK_EQ(t.nlines, 3);
    CHECK(strcmp(t.lines[0], "ab") == 0);
    CHECK(strcmp(t.lines[1], "xy") == 0);
    CHECK(strcmp(t.lines[2], "who") == 0);

    /* No echo (a password), except the line end. */
    memset(&t, 0, sizeof t);
    term_line_init(&l, false);
    type(&l, &t, "pw\x08x\r", 5);
    CHECK(strcmp(t.lines[0], "px") == 0);
    CHECK_BYTES(t.echo, t.echo_len, "0D0A");

    /* Too long: the rest is dropped. */
    char longline[TERM_LINE_MAX + 10u];
    memset(longline, 'a', sizeof longline);
    longline[sizeof longline - 1u] = '\r';
    memset(&t, 0, sizeof t);
    term_line_init(&l, false);
    type(&l, &t, longline, sizeof longline);
    CHECK_EQ(strlen(t.lines[0]), TERM_LINE_MAX);
}

static void command_words(void)
{
    char arg[16];
    CHECK_EQ(term_cmd_parse("connect d102", arg, sizeof arg), TERM_CMD_CONNECT);
    CHECK(strcmp(arg, "D102") == 0);
    CHECK_EQ(term_cmd_parse("  CoNnEcT   D100  ", arg, sizeof arg), TERM_CMD_CONNECT);
    CHECK(strcmp(arg, "D100") == 0);
    CHECK_EQ(term_cmd_parse("connect", arg, sizeof arg), TERM_CMD_UNKNOWN);
    CHECK_EQ(term_cmd_parse("connect a b", arg, sizeof arg), TERM_CMD_UNKNOWN);
    CHECK_EQ(term_cmd_parse("connect 12345678901234567890", arg, sizeof arg), TERM_CMD_UNKNOWN);
    CHECK_EQ(term_cmd_parse("list", arg, sizeof arg), TERM_CMD_LIST);
    CHECK_EQ(term_cmd_parse("Help", arg, sizeof arg), TERM_CMD_HELP);
    CHECK_EQ(term_cmd_parse("?", arg, sizeof arg), TERM_CMD_HELP);
    CHECK_EQ(term_cmd_parse("help copy-term", arg, sizeof arg), TERM_CMD_HELP);
    CHECK(strcmp(arg, "COPY-TERM") == 0); /* the command word, in upper case */
    CHECK_EQ(term_cmd_parse("? who", arg, sizeof arg), TERM_CMD_HELP);
    CHECK(strcmp(arg, "WHO") == 0);
    CHECK_EQ(term_cmd_parse("help a b", arg, sizeof arg), TERM_CMD_UNKNOWN);
    {
        /* term_line_append: what TAB completion adds, as if typed, within the line's room. */
        TermLine tl;
        term_line_init(&tl, true);
        CHECK_EQ(term_line_append(&tl, "cop"), 3);
        CHECK_EQ(term_line_append(&tl, "y-term "), 7);
        CHECK(strcmp(tl.text, "copy-term ") == 0 && tl.len == 10);
        CHECK_EQ(term_line_append(&tl, "a\tb"), 1); /* stops at a control byte */
        while (term_line_append(&tl, "xxxxxxxxxx") > 0)
        {
        }
        CHECK_EQ(tl.len, TERM_LINE_MAX);
        CHECK_EQ(term_line_append(NULL, "x"), 0);
    }
    CHECK_EQ(term_cmd_parse("who", arg, sizeof arg), TERM_CMD_WHO);
    CHECK_EQ(term_cmd_parse("INFO", arg, sizeof arg), TERM_CMD_INFO);
    CHECK_EQ(term_cmd_parse("exit", arg, sizeof arg), TERM_CMD_EXIT);
    CHECK_EQ(term_cmd_parse("logout", arg, sizeof arg), TERM_CMD_EXIT);
    CHECK_EQ(term_cmd_parse("QUIT", arg, sizeof arg), TERM_CMD_EXIT);
    CHECK_EQ(term_cmd_parse("exit now", arg, sizeof arg), TERM_CMD_UNKNOWN);
    CHECK_EQ(term_cmd_parse("frobnicate", arg, sizeof arg), TERM_CMD_UNKNOWN);
    CHECK_EQ(term_cmd_parse("", arg, sizeof arg), TERM_CMD_EMPTY);
    /* User commands: the name keeps its case; ADDUSER may add ADMIN. */
    CHECK_EQ(term_cmd_parse("password", arg, sizeof arg), TERM_CMD_PASSWORD);
    CHECK_EQ(term_cmd_parse("password x", arg, sizeof arg), TERM_CMD_UNKNOWN);
    CHECK_EQ(term_cmd_parse("Users", arg, sizeof arg), TERM_CMD_USERS);
    CHECK_EQ(term_cmd_parse("adduser Ronny", arg, sizeof arg), TERM_CMD_ADDUSER);
    CHECK(strcmp(arg, "Ronny") == 0);
    CHECK_EQ(term_cmd_parse("ADDUSER kari admin", arg, sizeof arg), TERM_CMD_ADDADMIN);
    CHECK(strcmp(arg, "kari") == 0);
    CHECK_EQ(term_cmd_parse("adduser kari Admin", arg, sizeof arg), TERM_CMD_ADDADMIN);
    CHECK_EQ(term_cmd_parse("adduser kari root", arg, sizeof arg), TERM_CMD_UNKNOWN);
    CHECK_EQ(term_cmd_parse("adduser kari admin x", arg, sizeof arg), TERM_CMD_UNKNOWN);
    CHECK_EQ(term_cmd_parse("adduser", arg, sizeof arg), TERM_CMD_UNKNOWN);
    CHECK_EQ(term_cmd_parse("deluser Kari", arg, sizeof arg), TERM_CMD_DELUSER);
    CHECK(strcmp(arg, "Kari") == 0);
    CHECK_EQ(term_cmd_parse("deluser kari admin", arg, sizeof arg), TERM_CMD_UNKNOWN);
    CHECK_EQ(term_cmd_parse("resetpw kari", arg, sizeof arg), TERM_CMD_RESETPW);
    CHECK_EQ(term_cmd_parse("resetpw", arg, sizeof arg), TERM_CMD_UNKNOWN);
    CHECK_EQ(term_cmd_parse("resetpw 12345678901234567890", arg, sizeof arg), TERM_CMD_UNKNOWN);
    CHECK_EQ(term_cmd_parse("   ", arg, sizeof arg), TERM_CMD_EMPTY);
}

/* ------------------------------------------------------------------ WHO and INFO */

static void who_text(void)
{
    const TermWhoEntry e[2] = {
        {1, "ronny", "127.0.0.1:50123", 3725, "D102", "running", true},
        {2, NULL, "10.0.0.7:4000", 90061, NULL, NULL, false},
    };
    char buf[512];
    int n = term_report_who(e, 2, buf, sizeof buf);
    CHECK(n > 0);
    CHECK(strcmp(buf, "  TERM USER             FROM                        CONNECTED    DOING\r\n"
                      "*    1 ronny            127.0.0.1:50123             01:02:05     "
                      "connected to D102 (running)\r\n"
                      "     2 (login)          10.0.0.7:4000               1d 01:01:01  "
                      "at the prompt\r\n") == 0);
    if (n > 0)
    {
        CHECK_EQ((size_t)n, strlen(buf));
    }
    CHECK_EQ(term_report_who(e, 2, buf, 40), -1);
}

static void info_text(void)
{
    TermLinkInfo li;
    char buf[1024];
    memset(&li, 0, sizeof li);
    li.own_node = 19999;
    li.own_system = 19999;
    li.peer_node_learned = -1;
    li.peer_node_cfg = 100;
    li.peer_system = "D100";
    li.transport = "HDLC over TCP 127.0.0.1:10362";
    li.line_is_tcp = true;
    li.peer_window = 4;
    li.uptime_s = 61;
    CHECK(term_report_info(&li, buf, sizeof buf) > 0);
    CHECK(strcmp(buf, "Local:   node 19999, system D19999\r\n"
                      "Remote:  node 100 (D100) configured; node from the link: unknown\r\n"
                      "Link:    down\r\n"
                      "Line:    HDLC over TCP 127.0.0.1:10362\r\n"
                      "Speed:   not known (TCP line)\r\n"
                      "Frames:  0 in, 0 out, 0 sent again, 0 with an unusual address\r\n"
                      "Window:  the peer's receive window is 4 (from its last address byte)\r\n"
                      "Data:    0 datagrams in, 0 out\r\n"
                      "Uptime:  00:01:01\r\n") == 0);
    li.peer_node_learned = 100;
    li.link_up = true;
    li.link_ready = true;
    li.line_bps = 9600;
    li.line_is_tcp = false;
    CHECK(term_report_info(&li, buf, sizeof buf) > 0);
    CHECK(strstr(buf, "node from the link: 100\r\n") != NULL);
    CHECK(strstr(buf, "Link:    up, ready\r\n") != NULL);
    CHECK(strstr(buf, "Speed:   9600 bits per second\r\n") != NULL);
    li.line_bps = 0;
    CHECK(term_report_info(&li, buf, sizeof buf) > 0);
    CHECK(strstr(buf, "Speed:   not known\r\n") != NULL);
    CHECK_EQ(term_report_info(&li, buf, 30), -1);
}

/* Feed socket bytes; collect the data bytes and the answers. */
static void tn_feed(TermTelnet *t, const char *hex, uint8_t *data, size_t *dn, uint8_t *ans,
                    size_t *an)
{
    uint8_t in[64];
    int n = test_hex_to_bytes(hex, in, sizeof in);
    *dn = 0;
    *an = 0;
    for (int i = 0; i < n; i++)
    {
        uint8_t r[TERM_TELNET_REPLY_MAX];
        size_t rl = 0;
        int d = term_telnet_rx(t, in[i], r, &rl);
        if (d >= 0)
        {
            data[(*dn)++] = (uint8_t)d;
        }
        memcpy(ans + *an, r, rl);
        *an += rl;
    }
}

static void telnet_codec(void)
{
    TermTelnet t;
    uint8_t out[32];
    uint8_t data[64];
    uint8_t ans[64];
    size_t dn;
    size_t an;
    size_t taken;
    static const uint8_t ff[] = {0x41, 0xFF, 0x42};

    /* Binary (the host tool): WILL ECHO, SGA, BINARY; DO SGA, BINARY. */
    term_telnet_init(&t, true);
    CHECK_EQ(term_telnet_open(&t, out, sizeof out), 15);
    CHECK_BYTES(out, 15, "FFFB01 FFFB03 FFFB00 FFFD03 FFFD00");
    CHECK_EQ(term_telnet_open(&t, out, 14), -1);
    /* Agreeing answers change nothing; refused options; a subnegotiation is skipped. */
    tn_feed(&t, "FFFD01 FFFB03 FFFD18 FFFB1F 41 FFFA1F0050FFF0 42", data, &dn, ans, &an);
    CHECK_BYTES(data, dn, "41 42");
    CHECK_BYTES(ans, an, "FFFC18 FFFE1F");
    /* IAC IAC is 0xFF; in binary CR NUL keeps its NUL. */
    tn_feed(&t, "FFFF 0D00", data, &dn, ans, &an);
    CHECK_BYTES(data, dn, "FF 0D 00");
    /* The client leaves binary: now CR NUL loses its NUL, CR LF stays. */
    tn_feed(&t, "FFFC00 0D00 0D0A", data, &dn, ans, &an);
    CHECK_BYTES(ans, an, "FFFE00");
    CHECK_BYTES(data, dn, "0D 0D 0A");
    CHECK(!term_telnet_is_on(&t, TERM_TN_BINARY, true));
    CHECK(term_telnet_is_on(&t, TERM_TN_ECHO, false));
    /* DONT ECHO turns our echo off and says so; DO again turns it back on. */
    tn_feed(&t, "FFFE01 FFFD01", data, &dn, ans, &an);
    CHECK_BYTES(ans, an, "FFFC01 FFFB01");

    /* Not binary (XMSGGW): no BINARY offered; a DO BINARY is refused. */
    term_telnet_init(&t, false);
    CHECK_EQ(term_telnet_open(&t, out, sizeof out), 9);
    CHECK_BYTES(out, 9, "FFFB01 FFFB03 FFFD03");
    tn_feed(&t, "FFFD00 FFFB00", data, &dn, ans, &an);
    CHECK_BYTES(ans, an, "FFFC00 FFFE00");

    /* Out: 0xFF doubled, never split. */
    CHECK_EQ(term_telnet_escape(ff, 3, out, sizeof out, &taken), 4);
    CHECK_BYTES(out, 4, "41 FF FF 42");
    CHECK_EQ(taken, 3);
    CHECK_EQ(term_telnet_escape(ff, 3, out, 2, &taken), 1);
    CHECK_EQ(taken, 1);
    CHECK_EQ(term_telnet_rx(NULL, 0, out, &taken), -2);
}

int main(void)
{
    printf("[term]\n");
    lone_esc_is_an_escape();
    function_key_passes_intact();
    esc_at_end_of_a_read_then_byte_soon();
    late_byte_after_an_unpolled_esc();
    two_esc_in_a_row();
    zero_means_always_data();
    keyseq_help_key_in_one_chunk();
    keyseq_help_key_split_with_small_gaps();
    keyseq_other_shapes();
    keyseq_unterminated();
    keyseq_esc_esc();
    keyseq_lone_esc();
    line_editing();
    command_words();
    who_text();
    info_text();
    telnet_codec();
    TEST_DONE();
}
