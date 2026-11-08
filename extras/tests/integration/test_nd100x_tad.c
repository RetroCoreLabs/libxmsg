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
 *
 * What it types, one line per input grant, as the real D100 user did in the captured session
 * (record 257 onward in RetroIO pcap-decode-sample.json): the user name, the password, then
 * LIST-FILES,,, and LOGOUT. What is checked: the session reaches RUNNING, every line goes out,
 * text comes back, and the host ends the session after LOGOUT, which our DCON answers (state
 * CLOSED). The whole run must finish within two minutes.
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

static void on_text(void *ctx, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    s_text_len += len;
    fwrite(bytes, 1, len, stdout);
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
    char lines[4][80];
    snprintf(lines[0], sizeof lines[0], "%s\r", env_str("ND100X_USER", "SYSTEM"));
    snprintf(lines[1], sizeof lines[1], "%s\r", env_str("ND100X_PASSWORD", ""));
    snprintf(lines[2], sizeof lines[2], "LIST-FILES,,,\r");
    snprintf(lines[3], sizeof lines[3], "LOGOUT\r");

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
    ports.log = sink;
    memset(&cb, 0, sizeof cb);
    CHECK_EQ(xmsg_node_init(&node, &cfg, &ports, &cb), XMSG_OK);

    static TadAsker asker;
    TadAskerConfig acfg;
    TadAskerCallbacks acb = {NULL, on_text, on_state, NULL, NULL};
    tad_asker_config_default(&acfg, peer, system);
    CHECK_EQ(tad_asker_init(&asker, &acfg, &acb), XMSG_OK);

    uint32_t start = test_now_ms();
    uint32_t now = start;
    bool connected = false;
    int next_line = 0;
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
        if (!connected && xmsg_node_is_ready(&node))
        {
            CHECK_EQ(tad_asker_connect(&asker, &node, now), XMSG_OK);
            connected = true;
        }
        if (next_line < 4 && tad_asker_may_type(&asker))
        {
            size_t n = strlen(lines[next_line]);
            CHECK_EQ(tad_asker_write(&asker, (const uint8_t *)lines[next_line], n, now), (int)n);
            next_line++;
        }
        if (asker.state == TAD_ASKER_CLOSED || asker.state == TAD_ASKER_FAILED)
        {
            break;
        }
        test_tcp_wait_readable(s_sock, 10);
    }

    CHECK(connected);
    CHECK_EQ(next_line, 4);
    CHECK_EQ(asker.state, TAD_ASKER_CLOSED);
    CHECK(s_text_len > 0);
    CHECK_EQ(node.stats.bad_headers, 0);
    printf("  %u text bytes, %u frames in, %u out, %u XENSE resends, %u ms\n", (unsigned)s_text_len,
           asker.stats.frames_in, asker.stats.frames_out, asker.stats.xense_resends,
           (unsigned)(now - start));
    xmsg_node_link_stop(&node, now);
    test_tcp_close(s_sock);
    TEST_DONE();
}
