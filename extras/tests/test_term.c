/*
 * test_term.c - the lone-ESC detector, the line editor, the command words and WHO/INFO text.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/term/term_escape.h"
#include "libxmsg/term/term_line.h"
#include "libxmsg/term/term_report.h"
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
    CHECK(strcmp(buf, "  TERM USER             FROM                   CONNECTED    DOING\r\n"
                      "*    1 ronny            127.0.0.1:50123        01:02:05     "
                      "connected to D102 (running)\r\n"
                      "     2 (login)          10.0.0.7:4000          1d 01:01:01  "
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

int main(void)
{
    printf("[term]\n");
    lone_esc_is_an_escape();
    function_key_passes_intact();
    esc_at_end_of_a_read_then_byte_soon();
    late_byte_after_an_unpolled_esc();
    two_esc_in_a_row();
    zero_means_always_data();
    line_editing();
    command_words();
    who_text();
    info_text();
    TEST_DONE();
}
