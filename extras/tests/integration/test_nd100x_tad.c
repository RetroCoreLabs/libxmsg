/*
 * test_nd100x_tad.c - log in to a running nd100x through a TAD session, list files, log out.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Needs an nd100x started with --hdlc=1:PORT (it listens), SINTRAN with XMSG running, and a link
 * defined there for our node. Environment:
 *   ND100X_HDLC_HOST   host:port of the line (unset: the test prints SKIPPED and exits with 2)
 *   ND100X_OUR_NODE    our node number (default 19999, as the LAPB integration test)
 *   ND100X_PEER_NODE   the machine's node number (default 100)
 *   ND100X_SYSTEM      its system name as XROUT knows it (default D100)
 *   ND100X_USER        user to log in as (default SYSTEM)
 *   ND100X_PASSWORD    password (default empty)
 *   ND100X_REACH_FIRST 1: send a reachability request (0x19) before connecting (default 0)
 *   ND100X_FLAGS1_FILE where our Flags1 count to the host is kept between runs (default
 *                      nd100x_tad.flags1 in the working directory; a line "peer next" and a
 *                      line "seed N" for the port-word generator, as host_tad_telnet keeps). Without it every run starts at 0, the
 *                      host still expects the number after our last frame, and it drops the
 *                      connect letter in silence (measured on RetroCore D100, 2026-11-09).
 *
 * What it types, one line per input grant, as the real D100 user did in the captured session
 * (record 257 onward in RetroIO pcap-decode-sample.json): the user name, the password, then
 * LIST-FILES,,, and LOGOUT, with TADA before LOGOUT. TADA lists every TAD terminal; for each
 * remote one, ours included, the host asks "who are you" (7WHO) and waits for our 7IAM, so the
 * listing reaches its end and the next prompt only if we answer (it hung on our row before).
 * What is checked: the session reaches RUNNING, every line goes out (LOGOUT can only go out
 * after TADA has finished), text comes back, TADA's header and our own system name in its
 * listing are seen, at least one 7WHO was answered, the host sends its session notice (0xFD) after LOGOUT, and we then hang up
 * with DCON, as a real asker does (state CLOSED). A notice that comes BEFORE LOGOUT (D100 sends
 * one ~3 s after connect) must not end anything. The whole run must finish within two minutes.
 */
#include "libxmsg/log/xmsg_log.h"
#include "libxmsg/tad/tad_asker.h"
#include "libxmsg/xmsg/xmsg_node.h"

#include "pair/test_pair.h"
#include "test_util.h"

static void log_sink(void *ctx, XmsgLogCategory category, XmsgLogLevel level, const char *line)
{
    (void)ctx;
    printf("  [%d/%d] %s\n", (int)category, (int)level, line);
}

static size_t s_text_len;

/* Looks for one string in the host text as it streams past, across frame boundaries. */
typedef struct TextMatch
{
    const char *want;
    size_t at;
    bool seen;
} TextMatch;

static TextMatch s_tada_header = {"TAD/TYP RESERV", 0, false};
static TextMatch s_our_name = {NULL, 0, false}; /* "D" + our node, set in main */

static void match_feed(TextMatch *m, const uint8_t *bytes, size_t len)
{
    if (m->want == NULL || m->seen)
    {
        return;
    }
    size_t wlen = strlen(m->want);
    for (size_t i = 0; i < len && !m->seen; i++)
    {
        char c = (char)(bytes[i] & 0x7Fu);
        if (c == m->want[m->at])
        {
            m->at++;
        }
        else
        {
            m->at = (c == m->want[0]) ? 1u : 0u;
        }
        if (m->at == wlen)
        {
            m->seen = true;
        }
    }
}

static void on_text(void *ctx, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    s_text_len += len;
    match_feed(&s_tada_header, bytes, len);
    match_feed(&s_our_name, bytes, len);
    fwrite(bytes, 1, len, stdout);
}

static unsigned s_notices;

static void on_notice(void *ctx)
{
    (void)ctx;
    s_notices++;
    printf("\n  host session notice %u\n", s_notices);
}

static void on_state(void *ctx, TadAskerState st, int error)
{
    (void)ctx;
    printf("\n  session %s (error %d)\n", tad_asker_state_name(st), error);
}

static bool parse_host(const char *spec, char *host, size_t host_cap, uint16_t *port)
{
    const char *colon = strrchr(spec, ':');
    char *end = NULL;
    long p;
    if (colon == NULL || (size_t)(colon - spec) >= host_cap)
    {
        return false;
    }
    memcpy(host, spec, (size_t)(colon - spec));
    host[colon - spec] = '\0';
    p = strtol(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0' || p < 1 || p > 65535)
    {
        return false;
    }
    *port = (uint16_t)p;
    return true;
}

static uint16_t env_u16(const char *name, uint16_t fallback)
{
    const char *v = getenv(name);
    return (v != NULL) ? (uint16_t)strtol(v, NULL, 10) : fallback;
}

static const char *env_str(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    return (v != NULL) ? v : fallback;
}

static TestSocket s_sock;

/* The Flags1 file: "node N" (our own node number), "peer next" for the one host this test talks
 * to, and "seed N" for the port-word generator. All are read at start and the whole file is
 * written on every change.
 * The first line, "node N", names OUR node number. The counts are a host's view of US, so a file
 * written while we were another node number must not be used: on 2026-11-09 a count kept for
 * node 19999 was used for node 101 and D100 then dropped node 101's frames in silence. A file
 * with another node number is ignored (with a message); a file with no "node" line is taken
 * as ours. */
typedef struct FileStore
{
    const char *path;
    unsigned own;
    unsigned peer;
    unsigned next;
    unsigned seed;
    bool has_next;
    bool has_seed;
} FileStore;

static void store_read(FileStore *st)
{
    FILE *f = fopen(st->path, "r");
    char word[16];
    unsigned v;
    if (f == NULL)
    {
        return;
    }
    while (fscanf(f, "%15s %u", word, &v) == 2)
    {
        if (strcmp(word, "node") == 0)
        {
            if (v != st->own)
            {
                printf("  %s was written for node %u, we are node %u: its counts are not used\n",
                       st->path, v, st->own);
                st->has_next = false;
                st->has_seed = false;
                break;
            }
        }
        else if (strcmp(word, "seed") == 0)
        {
            st->seed = v;
            st->has_seed = true;
        }
        else
        {
            st->peer = (unsigned)strtoul(word, NULL, 10);
            st->next = v;
            st->has_next = true;
        }
    }
    fclose(f);
}

static int store_write(const FileStore *st)
{
    FILE *f = fopen(st->path, "w");
    if (f == NULL)
    {
        return -1;
    }
    fprintf(f, "node %u\n", st->own);
    if (st->has_next)
    {
        fprintf(f, "%u %u\n", st->peer, st->next);
    }
    if (st->has_seed)
    {
        fprintf(f, "seed %u\n", st->seed);
    }
    return (fclose(f) == 0) ? 0 : -1;
}

static int store_load(void *ctx, uint16_t peer, uint16_t *next)
{
    const FileStore *st = (const FileStore *)ctx;
    if (!st->has_next || st->peer != peer)
    {
        return -1;
    }
    *next = (uint16_t)st->next;
    return 0;
}

static int store_save(void *ctx, uint16_t peer, uint16_t next)
{
    FileStore *st = (FileStore *)ctx;
    st->peer = peer;
    st->next = next;
    st->has_next = true;
    return store_write(st);
}

static int store_load_seed(void *ctx, uint16_t *seed)
{
    const FileStore *st = (const FileStore *)ctx;
    if (!st->has_seed)
    {
        return -1;
    }
    *seed = (uint16_t)st->seed;
    return 0;
}

static int store_save_seed(void *ctx, uint16_t seed)
{
    FileStore *st = (FileStore *)ctx;
    st->seed = seed;
    st->has_seed = true;
    return store_write(st);
}

static int line_write(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    int r = test_tcp_write(s_sock, b, n);
    return (r < 0) ? XMSG_ESTATE : r;
}

int main(void)
{
    const char *spec = getenv("ND100X_HDLC_HOST");
    char host[128];
    uint16_t port;
    if (spec == NULL || !parse_host(spec, host, sizeof host, &port))
    {
        printf("SKIPPED: set ND100X_HDLC_HOST=host:port to run against nd100x --hdlc\n");
        return 2;
    }
    uint16_t own = env_u16("ND100X_OUR_NODE", 19999);
    uint16_t peer = env_u16("ND100X_PEER_NODE", 100);
    const char *system = env_str("ND100X_SYSTEM", "D100");
    enum
    {
        N_LINES = 5
    };
    char lines[N_LINES][80];
    snprintf(lines[0], sizeof lines[0], "%s\r", env_str("ND100X_USER", "SYSTEM"));
    snprintf(lines[1], sizeof lines[1], "%s\r", env_str("ND100X_PASSWORD", ""));
    snprintf(lines[2], sizeof lines[2], "LIST-FILES,,,\r");
    snprintf(lines[3], sizeof lines[3], "TADA\r");
    snprintf(lines[4], sizeof lines[4], "LOGOUT\r");
    static char s_name[8];
    snprintf(s_name, sizeof s_name, "D%u", (unsigned)own);
    s_our_name.want = s_name;

    XmsgLogPort sink = {NULL, log_sink};
    xmsg_log_set_sink(&sink);
    xmsg_log_set_all_levels(XMSG_LOG_INFO);
    printf("[nd100x_tad] %s:%u as node %u, host node %u (%s)\n", host, (unsigned)port,
           (unsigned)own, (unsigned)peer, system);

    CHECK_EQ(test_tcp_startup(), 0);
    s_sock = test_tcp_connect_host(host, port);
    if (s_sock == -1)
    {
        printf("cannot connect to %s:%u\n", host, (unsigned)port);
        CHECK(false);
        TEST_DONE();
    }

    static XmsgNode node;
    XmsgNodeConfig cfg;
    XmsgPorts ports;
    XmsgNodeCallbacks cb;
    xmsg_node_config_default(&cfg, own);
    memset(&ports, 0, sizeof ports);
    ports.transport.write = line_write;
    static FileStore store;
    store.path = env_str("ND100X_FLAGS1_FILE", "nd100x_tad.flags1");
    store.own = own;
    store_read(&store);
    ports.flags1.ctx = &store;
    ports.flags1.load = store_load;
    ports.flags1.save = store_save;
    ports.flags1.load_port_seed = store_load_seed;
    ports.flags1.save_port_seed = store_save_seed;
    ports.log = sink;
    memset(&cb, 0, sizeof cb);
    CHECK_EQ(xmsg_node_init(&node, &cfg, &ports, &cb), XMSG_OK);

    static TadAsker asker;
    TadAskerConfig acfg;
    TadAskerCallbacks acb = {NULL, on_text, on_state, NULL, NULL, on_notice};
    tad_asker_config_default(&acfg, peer, system);
    CHECK_EQ(tad_asker_init(&asker, &acfg, &acb), XMSG_OK);

    uint32_t start = test_now_ms();
    uint32_t now = start;
    bool connected = false;
    bool reach_first = env_u16("ND100X_REACH_FIRST", 0) != 0;
    bool reach_sent = false;
    uint32_t reach_at = 0;
    int next_line = 0;
    unsigned notices_at_logout = 0;
    uint8_t chunk[2048];
    xmsg_node_link_start(&node, now);

    while (now - start < 120000)
    {
        now = test_now_ms();
        int r = test_tcp_read(s_sock, chunk, sizeof chunk);
        if (r < 0)
        {
            printf("  the line closed\n");
            break;
        }
        if (r > 0)
        {
            xmsg_node_feed(&node, chunk, (size_t)r, now);
        }
        xmsg_node_tick(&node, now);
        /* ND100X_REACH_FIRST=1: send our own reachability request (0x19) when the link is
         * ready and connect one second later. A measurement switch: does the host start its
         * count for us again when WE announce ourselves? */
        if (reach_first && !reach_sent && xmsg_node_is_ready(&node))
        {
            CHECK_EQ(xmsg_node_send_reach(&node, peer, now), XMSG_OK);
            reach_sent = true;
            reach_at = now;
        }
        if (!connected && xmsg_node_is_ready(&node) && (!reach_first || now - reach_at >= 1000))
        {
            CHECK_EQ(tad_asker_connect(&asker, &node, now), XMSG_OK);
            connected = true;
        }
        if (next_line < N_LINES && tad_asker_may_type(&asker))
        {
            size_t n = strlen(lines[next_line]);
            CHECK_EQ(tad_asker_write(&asker, (const uint8_t *)lines[next_line], n, now), (int)n);
            next_line++;
            if (next_line == N_LINES)
            {
                notices_at_logout = s_notices;
            }
        }
        /* After LOGOUT, the host's notice says the login is over: our user is done, hang up. */
        if (next_line == N_LINES && asker.state == TAD_ASKER_RUNNING &&
            s_notices > notices_at_logout)
        {
            CHECK_EQ(tad_asker_hangup(&asker, now), XMSG_OK);
        }
        if (asker.state == TAD_ASKER_CLOSED || asker.state == TAD_ASKER_FAILED)
        {
            break;
        }
        test_tcp_wait_readable(s_sock, 10);
    }

    CHECK(connected);
    CHECK_EQ(next_line, N_LINES);
    CHECK(s_tada_header.seen);
    CHECK(s_our_name.seen);
    CHECK(asker.stats.who_answers >= 1);
    CHECK_EQ(asker.state, TAD_ASKER_CLOSED);
    CHECK(s_text_len > 0);
    CHECK_EQ(node.stats.bad_headers, 0);
    printf("  %u text bytes, %u frames in, %u out, %u XENSE resends, %u who-are-you answers, %u "
           "ms\n",
           (unsigned)s_text_len, asker.stats.frames_in, asker.stats.frames_out,
           asker.stats.xense_resends, asker.stats.who_answers, (unsigned)(now - start));
    xmsg_node_link_stop(&node, now);
    test_tcp_close(s_sock);
    TEST_DONE();
}
