/*
 * test_nd100x_lapb.c - bring the LAPB link up against a running nd100x and hold it.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Needs an nd100x started with --hdlc=1:PORT (it listens) and SINTRAN with XMSG running, so the
 * machine answers our SABM and sends its own. The address comes from the environment variable
 * ND100X_HDLC_HOST, as "host:port"; when it is unset the test prints SKIPPED and exits with code 2,
 * which CTest treats as a skip. Our node number is ND100X_OUR_NODE (default 19999, the number
 * D100 knows the runner by). Nothing above LAPB is sent: this test proves the link only.
 *
 * What is checked: the link comes up within 30 seconds; it then stays up for 70 seconds (or
 * ND100X_HOLD_MS) with zero retransmissions, and in that time exactly one T3 keep-alive poll goes
 * out. The ND machine does not answer an idle RR poll, so T3 is not re-armed and no second poll
 * follows (measured on RetroCore D100 port 10362 on 2026-11-09: poll at 30.3 s, nothing back in
 * the next 40 s; the C# RetroIO.Lapb LapbLayer records the same live observation). Last, the
 * DISC: it is sent again every T1, and the link must reach DISCONNECTED. The ND machine does not
 * answer a DISC at all - measured on RetroCore D100 on 2026-11-09: eleven DISCs, 3 s apart, no
 * UA and no DM - so our side gets there by N2 running out ("N2 exhausted during teardown"),
 * which is the ordinary LAPB rule for an unanswered DISC.
 */
#include "libhdlc/byte_frame.h"
#include "libxmsg/lapb/lapb.h"
#include "libxmsg/log/xmsg_log.h"

#include "pair/test_pair.h"
#include "test_util.h"

static void log_sink(void *ctx, XmsgLogCategory category, XmsgLogLevel level, const char *line)
{
    (void)ctx;
    printf("  [%d/%d] %s\n", (int)category, (int)level, line);
}

static int s_downs = 0;
static void on_state(void *ctx, LapbState state, bool up, const char *reason)
{
    (void)ctx;
    printf("  link %s up=%d (%s)\n", lapb_state_name(state), up ? 1 : 0, reason);
    if (state == LAPB_DISCONNECTED)
    {
        s_downs++;
    }
}

static int s_infos = 0;
static void on_info(void *ctx, const uint8_t *info, size_t len)
{
    (void)ctx;
    s_infos++;
    printf("  info field of %u bytes:", (unsigned)len);
    for (size_t i = 0; i < len && i < 16; i++)
    {
        printf(" %02X", info[i]);
    }
    printf("%s\n", len > 16 ? " ..." : "");
}

static bool parse_host(const char *spec, char *host, size_t host_cap, uint16_t *port)
{
    const char *colon = strrchr(spec, ':');
    if (colon == NULL || (size_t)(colon - spec) >= host_cap)
    {
        return false;
    }
    memcpy(host, spec, (size_t)(colon - spec));
    host[colon - spec] = '\0';
    char *end = NULL;
    long p = strtol(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0' || p < 1 || p > 65535)
    {
        return false;
    }
    *port = (uint16_t)p;
    return true;
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
    const char *node_env = getenv("ND100X_OUR_NODE");
    uint16_t own_node = (uint16_t)((node_env != NULL) ? strtol(node_env, NULL, 10) : 19999);
    const char *hold_env = getenv("ND100X_HOLD_MS");
    uint32_t hold_ms = (uint32_t)((hold_env != NULL) ? strtol(hold_env, NULL, 10) : 70000);

    XmsgLogPort sink = {NULL, log_sink};
    xmsg_log_set_sink(&sink);
    xmsg_log_set_all_levels(XMSG_LOG_DEBUG);

    printf("[nd100x_lapb] %s:%u as node %u\n", host, (unsigned)port, (unsigned)own_node);
    TestSocket sock = test_tcp_connect_host(host, port);
    if (sock == -1)
    {
        printf("cannot connect to %s:%u\n", host, (unsigned)port);
        CHECK(false);
        TEST_DONE();
    }

    LapbLink link;
    LapbConfig cfg;
    lapb_config_default(&cfg, own_node);
    CHECK_EQ(lapb_init(&link, &cfg, NULL, on_info, on_state), XMSG_OK);
    byte_frame_rx dec;
    static uint8_t rx_buf[XMSG_LAPB_N1 + 4];
    byte_frame_rx_init(&dec, rx_buf, sizeof rx_buf, true);
    uint8_t frame[XMSG_LAPB_N1 + 2];
    uint8_t wire[2 * (XMSG_LAPB_N1 + 4) + 2];
    uint8_t chunk[1024];

    uint32_t start = test_now_ms();
    uint32_t now = start;
    uint32_t up_at = 0;
    lapb_connect(&link, now);

    for (;;)
    {
        now = test_now_ms();
        lapb_tick(&link, now);
        int n;
        while ((n = lapb_pull_tx(&link, frame, sizeof frame)) > 0)
        {
            size_t w = byte_frame_encode_accm(frame, (size_t)n, BYTE_FRAME_ACCM_NONE, true, wire, sizeof wire);
            printf("  TX %02X %02X (%d bytes)\n", frame[0], frame[1], n);
            size_t off = 0;
            while (off < w)
            {
                int k = test_tcp_write(sock, wire + off, w - off);
                if (k < 0)
                {
                    printf("socket write failed\n");
                    CHECK(false);
                    TEST_DONE();
                }
                off += (size_t)k;
            }
        }
        int r = test_tcp_read(sock, chunk, sizeof chunk);
        if (r < 0)
        {
            printf("socket closed by the peer\n");
            CHECK(false);
            break;
        }
        size_t off = 0;
        while (off < (size_t)r)
        {
            size_t used = 0;
            byte_frame_rx_status st = byte_frame_rx_feed(&dec, chunk + off, (size_t)r - off, &used);
            off += used;
            if (st == BYTE_FRAME_RX_FRAME)
            {
                size_t fl;
                const uint8_t *f = byte_frame_rx_frame(&dec, &fl);
                printf("  RX %02X %02X (%u bytes)\n", f[0], f[1], (unsigned)fl);
                lapb_rx_frame(&link, f, fl, now);
            }
            else if (st != BYTE_FRAME_RX_NEED_MORE)
            {
                printf("  framing: status %d\n", (int)st);
            }
        }
        if (up_at == 0 && lapb_is_up(&link))
        {
            up_at = now;
            printf("  link up after %u ms\n", (unsigned)(now - start));
        }
        if (up_at == 0 && now - start > 30000)
        {
            printf("  link did not come up within 30 s\n");
            CHECK(false);
            break;
        }
        if (up_at != 0 && now - up_at >= hold_ms)
        {
            break;
        }
        if (lapb_state(&link) == LAPB_DISCONNECTED && up_at != 0)
        {
            printf("  link went down\n");
            CHECK(false);
            break;
        }
        test_tcp_wait_readable(sock, 20);
    }

    if (up_at != 0)
    {
        CHECK(lapb_is_up(&link));
        CHECK_EQ(link.stats.retransmissions, 0);
        /* One poll per idle T3 period, and none after it: the machine ignores the poll. */
        CHECK_EQ(link.stats.t3_polls, (hold_ms > 30000) ? 1 : 0);
        printf("  held %u ms: %u frames in, %u out, %u T3 polls, %u retransmissions, %u info\n",
               (unsigned)(now - up_at), link.stats.frames_in, link.stats.frames_out,
               link.stats.t3_polls, link.stats.retransmissions, (unsigned)s_infos);
        /* Polite teardown. */
        lapb_disconnect(&link, now);
        int n;
        while ((n = lapb_pull_tx(&link, frame, sizeof frame)) > 0)
        {
            size_t w = byte_frame_encode_accm(frame, (size_t)n, BYTE_FRAME_ACCM_NONE, true, wire, sizeof wire);
            (void)test_tcp_write(sock, wire, w);
        }
        /* Wait with T1 running, so the DISC is sent again every T1 as LAPB (and the C#
         * LapbLayer.Disconnect) does, up to N2 times. */
        uint32_t t0 = test_now_ms();
        uint32_t wait_ms = link.cfg.t1_ms * (uint32_t)(link.cfg.n2 + 2u);
        while (lapb_state(&link) != LAPB_DISCONNECTED && test_now_ms() - t0 < wait_ms)
        {
            lapb_tick(&link, test_now_ms());
            while ((n = lapb_pull_tx(&link, frame, sizeof frame)) > 0)
            {
                size_t w = byte_frame_encode_accm(frame, (size_t)n, BYTE_FRAME_ACCM_NONE, true, wire, sizeof wire);
                (void)test_tcp_write(sock, wire, w);
            }
            test_tcp_wait_readable(sock, 50);
            int r = test_tcp_read(sock, chunk, sizeof chunk);
            size_t off = 0;
            while (r > 0 && off < (size_t)r)
            {
                size_t used = 0;
                if (byte_frame_rx_feed(&dec, chunk + off, (size_t)r - off, &used) ==
                    BYTE_FRAME_RX_FRAME)
                {
                    size_t fl;
                    const uint8_t *f = byte_frame_rx_frame(&dec, &fl);
                    lapb_rx_frame(&link, f, fl, test_now_ms());
                }
                off += used;
            }
        }
        printf("  after DISC: %s, %u frames in\n", lapb_state_name(lapb_state(&link)),
               link.stats.frames_in);
        CHECK_EQ(lapb_state(&link), LAPB_DISCONNECTED);
    }
    test_tcp_close(sock);
    TEST_DONE();
}
