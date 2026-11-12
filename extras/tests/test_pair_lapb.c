/*
 * test_pair_lapb.c - two LAPB stations bring a link up and move data through a faulty wire.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Each station is the stack a node will have: decoder -> LAPB -> encoder -> transport. Both call
 * connect at once (the ND balanced bring-up), then each sends numbered messages while the wire
 * drops, duplicates and corrupts frames and splits reads. Every message must arrive exactly once
 * and in order at the far end; the link must never go down. Then the same over loopback TCP with
 * a clean wire and real time.
 */
#include "libhdlc/byte_frame.h"
#include "libxmsg/lapb/lapb.h"

#include "pair/test_pair.h"
#include "test_util.h"

#define MESSAGES 300

typedef struct Station
{
    LapbLink lapb;
    byte_frame_rx dec;
    uint8_t rx_buf[XMSG_LAPB_N1 + 4];
    uint8_t tx_frame[XMSG_LAPB_N1 + 2];
    uint8_t tx_wire[2 * (XMSG_LAPB_N1 + 4) + 2];
    TestPair *pair;
    int side;
    TestSocket sock;
    unsigned next_send;     /**< sequence number of the next message to send */
    unsigned next_expected; /**< sequence number the next received message must carry */
    unsigned received;
    unsigned misordered;
    unsigned corrupt_payloads;
    int downs;
} Station;

static Station s_a;
static Station s_b;

static size_t build_message(unsigned seq, uint8_t *out)
{
    size_t len = 1 + (seq * 53) % 600; /* 1..600 bytes, odd and even lengths */
    out[0] = (uint8_t)(seq >> 8);
    out[1] = (uint8_t)seq;
    if (len < 2)
    {
        len = 2;
    }
    for (size_t i = 2; i < len; i++)
    {
        out[i] = (uint8_t)((i % 9 == 0) ? 0x7E : (i % 17 == 0) ? 0x7D : (seq * 3 + i));
    }
    return len;
}

static void station_on_info(void *ctx, const uint8_t *info, size_t len)
{
    Station *s = (Station *)ctx;
    uint8_t expect[700];
    if (len < 2)
    {
        s->corrupt_payloads++;
        return;
    }
    unsigned seq = ((unsigned)info[0] << 8) | info[1];
    size_t el = build_message(seq, expect);
    if (el != len || memcmp(expect, info, len) != 0)
    {
        s->corrupt_payloads++;
    }
    if (seq != s->next_expected)
    {
        s->misordered++;
    }
    s->next_expected = seq + 1;
    s->received++;
}

static void station_on_state(void *ctx, LapbState state, bool up, const char *reason)
{
    Station *s = (Station *)ctx;
    (void)reason;
    if (state == LAPB_DISCONNECTED && !up)
    {
        s->downs++;
    }
}

static void station_init(Station *s, uint16_t node, TestPair *pair, int side, TestSocket sock,
                         uint32_t t1_ms)
{
    LapbConfig cfg;
    lapb_config_default(&cfg, node);
    cfg.t1_ms = t1_ms;
    cfg.t3_ms = t1_ms * 10;
    lapb_init(&s->lapb, &cfg, s, station_on_info, station_on_state);
    byte_frame_rx_init(&s->dec, s->rx_buf, sizeof s->rx_buf, true);
    s->pair = pair;
    s->side = side;
    s->sock = sock;
    s->next_send = 0;
    s->next_expected = 0;
    s->received = 0;
    s->misordered = 0;
    s->corrupt_payloads = 0;
    s->downs = 0;
}

static void station_write(Station *s, const uint8_t *bytes, size_t len)
{
    if (s->pair != NULL)
    {
        CHECK_EQ(test_pair_write(s->pair, s->side, bytes, len), (int)len);
        return;
    }
    size_t off = 0;
    while (off < len)
    {
        int k = test_tcp_write(s->sock, bytes + off, len - off);
        CHECK(k >= 0);
        if (k <= 0)
        {
            test_sleep_ms(1);
            continue;
        }
        off += (size_t)k;
    }
}

/* Pull every due LAPB frame, frame it, write it. */
static void station_pump_tx(Station *s)
{
    for (;;)
    {
        int n = lapb_pull_tx(&s->lapb, s->tx_frame, sizeof s->tx_frame);
        CHECK(n >= 0);
        if (n <= 0)
        {
            return;
        }
        size_t w = byte_frame_encode_accm(s->tx_frame, (size_t)n, BYTE_FRAME_ACCM_NONE, true,
                                          s->tx_wire, sizeof s->tx_wire);
        CHECK(w > 0);
        station_write(s, s->tx_wire, w);
    }
}

/* Read whatever arrived, decode, hand frames to LAPB. */
static void station_pump_rx(Station *s, uint32_t now)
{
    uint8_t chunk[512];
    for (;;)
    {
        size_t n;
        if (s->pair != NULL)
        {
            n = test_pair_read(s->pair, s->side, chunk, sizeof chunk);
        }
        else
        {
            int r = test_tcp_read(s->sock, chunk, sizeof chunk);
            CHECK(r >= 0);
            n = (r > 0) ? (size_t)r : 0;
        }
        if (n == 0)
        {
            return;
        }
        size_t off = 0;
        while (off < n)
        {
            size_t used = 0;
            byte_frame_rx_status st = byte_frame_rx_feed(&s->dec, chunk + off, n - off, &used);
            off += used;
            if (st == BYTE_FRAME_RX_FRAME)
            {
                size_t fl;
                const uint8_t *f = byte_frame_rx_frame(&s->dec, &fl);
                lapb_rx_frame(&s->lapb, f, fl, now);
                station_pump_tx(s);
            }
        }
    }
}

/* Offer the next message if the window has room. */
static void station_offer(Station *s, uint32_t now)
{
    while (s->next_send < MESSAGES)
    {
        uint8_t *info;
        size_t cap;
        if (lapb_tx_reserve(&s->lapb, &info, &cap) != XMSG_OK)
        {
            return;
        }
        size_t len = build_message(s->next_send, info);
        CHECK(len <= cap);
        CHECK_EQ(lapb_tx_commit(&s->lapb, len, now), XMSG_OK);
        s->next_send++;
    }
}

static void run_in_memory(const TestFaults *fa, const TestFaults *fb, const char *label)
{
    static TestPair pair;
    test_pair_init(&pair);
    test_pair_set_faults(&pair, 0, fa);
    test_pair_set_faults(&pair, 1, fb);
    station_init(&s_a, 100, &pair, 0, -1, 50);
    station_init(&s_b, 102, &pair, 1, -1, 50);

    uint32_t now = 0;
    lapb_connect(&s_a.lapb, now);
    lapb_connect(&s_b.lapb, now);
    station_pump_tx(&s_a);
    station_pump_tx(&s_b);

    /* Simulated time: 1 ms per step; the stations tick, pump and offer every step. */
    while ((s_a.received < MESSAGES || s_b.received < MESSAGES) && now < 600000)
    {
        now += 1;
        lapb_tick(&s_a.lapb, now);
        lapb_tick(&s_b.lapb, now);
        station_pump_tx(&s_a);
        station_pump_tx(&s_b);
        station_pump_rx(&s_a, now);
        station_pump_rx(&s_b, now);
        if (lapb_is_up(&s_a.lapb))
        {
            station_offer(&s_a, now);
        }
        if (lapb_is_up(&s_b.lapb))
        {
            station_offer(&s_b, now);
        }
        station_pump_tx(&s_a);
        station_pump_tx(&s_b);
    }

    CHECK_EQ(s_a.received, MESSAGES);
    CHECK_EQ(s_b.received, MESSAGES);
    CHECK_EQ(s_a.misordered, 0);
    CHECK_EQ(s_b.misordered, 0);
    CHECK_EQ(s_a.corrupt_payloads, 0);
    CHECK_EQ(s_b.corrupt_payloads, 0);
    CHECK_EQ(s_a.downs, 0);
    CHECK_EQ(s_b.downs, 0);
    CHECK(lapb_is_up(&s_a.lapb));
    CHECK(lapb_is_up(&s_b.lapb));
    printf("  %s: %u ms, a<-b %u msgs (retx %u, rej sent %u, dup %u, fcs err %u), b<-a %u msgs "
           "(retx %u, rej sent %u, dup %u, fcs err %u)\n",
           label, (unsigned)now, s_a.received, s_b.lapb.stats.retransmissions,
           s_a.lapb.stats.rej_sent, s_a.lapb.stats.duplicates, s_a.dec.stats.fcs_errors,
           s_b.received, s_a.lapb.stats.retransmissions, s_b.lapb.stats.rej_sent,
           s_b.lapb.stats.duplicates, s_b.dec.stats.fcs_errors);
}

static void clean_wire(void)
{
    run_in_memory(NULL, NULL, "clean wire");
    /* With nothing lost there must be no retransmission at all. */
    CHECK_EQ(s_a.lapb.stats.retransmissions, 0);
    CHECK_EQ(s_b.lapb.stats.retransmissions, 0);
}

static void faulty_wire(void)
{
    TestFaults fa = {11, 0, 13, 7,
                     false}; /* a->b: drop every 11th, corrupt every 13th, 7-byte reads */
    TestFaults fb = {0, 9, 17, 3, false}; /* b->a: duplicate every 9th, corrupt every 17th */
    run_in_memory(&fa, &fb, "faulty wire");
    CHECK(s_a.lapb.stats.retransmissions + s_b.lapb.stats.retransmissions > 0);
}

static void lossy_both_ways(void)
{
    TestFaults f = {5, 6, 7, 1, false}; /* drop, duplicate and corrupt often; 1-byte reads */
    run_in_memory(&f, &f, "lossy both ways");
}

static void over_loopback_tcp(void)
{
    TestTcpPair tcp;
    if (test_tcp_pair_open(&tcp) != 0)
    {
        CHECK(false);
        return;
    }
    station_init(&s_a, 100, NULL, 0, tcp.sock[0], 300);
    station_init(&s_b, 102, NULL, 1, tcp.sock[1], 300);
    uint32_t start = test_now_ms();
    uint32_t now = start;
    lapb_connect(&s_a.lapb, now);
    lapb_connect(&s_b.lapb, now);
    station_pump_tx(&s_a);
    station_pump_tx(&s_b);
    while ((s_a.received < MESSAGES || s_b.received < MESSAGES) && (now - start) < 20000)
    {
        now = test_now_ms();
        lapb_tick(&s_a.lapb, now);
        lapb_tick(&s_b.lapb, now);
        station_pump_rx(&s_a, now);
        station_pump_rx(&s_b, now);
        if (lapb_is_up(&s_a.lapb))
        {
            station_offer(&s_a, now);
        }
        if (lapb_is_up(&s_b.lapb))
        {
            station_offer(&s_b, now);
        }
        station_pump_tx(&s_a);
        station_pump_tx(&s_b);
        if (!test_tcp_wait_readable(tcp.sock[0], 1))
        {
            test_tcp_wait_readable(tcp.sock[1], 1);
        }
    }
    CHECK_EQ(s_a.received, MESSAGES);
    CHECK_EQ(s_b.received, MESSAGES);
    CHECK_EQ(s_a.misordered, 0);
    CHECK_EQ(s_b.misordered, 0);
    CHECK_EQ(s_a.downs, 0);
    CHECK_EQ(s_b.downs, 0);
    printf("  tcp loopback: %u ms for %u messages each way\n", (unsigned)(now - start), MESSAGES);
    test_tcp_pair_close(&tcp);
}

int main(void)
{
    printf("[pair_lapb]\n");
    clean_wire();
    faulty_wire();
    lossy_both_ways();
    over_loopback_tcp();
    TEST_DONE();
}
