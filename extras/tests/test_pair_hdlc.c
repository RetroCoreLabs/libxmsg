/*
 * test_pair_hdlc.c - two framers talk to each other: in memory with faults, and over loopback TCP.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * The first peer-to-peer test. Each side encodes numbered frames of varying size and feeds what
 * it reads to its decoder. With the in-memory pair the reads are split into tiny chunks and every
 * tenth write is corrupted, so the FCS must catch exactly those and every other frame must arrive
 * intact and in order. Then the same traffic runs over a real socket pair on 127.0.0.1, which is
 * how a test on two separate processes will look later.
 */
#include "libhdlc/byte_frame.h"

#include "pair/test_pair.h"
#include "test_util.h"

#define FRAMES_PER_SIDE 400

static TestPair s_pair;
static TestTcpPair s_tcp;

typedef struct Side
{
    byte_frame_rx decoder;
    uint8_t rx_buf[XMSG_LAPB_N1 + 4];
    unsigned next_expected; /**< sequence number the next good frame must carry */
    unsigned received;
    unsigned out_of_order;
} Side;

/* A frame: 2-byte sequence, then (seq % 600) filler bytes with a pattern that includes 7E/7D. */
static size_t build_frame(unsigned seq, uint8_t *out)
{
    size_t len = 2 + (seq * 37) % 600;
    out[0] = (uint8_t)(seq >> 8);
    out[1] = (uint8_t)seq;
    for (size_t i = 2; i < len; i++)
    {
        out[i] = (uint8_t)((i % 11 == 0) ? 0x7E : (i % 13 == 0) ? 0x7D : (seq + i));
    }
    return len;
}

static void side_init(Side *s)
{
    byte_frame_rx_init(&s->decoder, s->rx_buf, sizeof s->rx_buf, true);
    s->next_expected = 0;
    s->received = 0;
    s->out_of_order = 0;
}

static void side_consume(Side *s, const uint8_t *bytes, size_t n)
{
    size_t off = 0;
    while (off < n)
    {
        size_t used = 0;
        byte_frame_rx_status st = byte_frame_rx_feed(&s->decoder, bytes + off, n - off, &used);
        off += used;
        if (st == BYTE_FRAME_RX_FRAME)
        {
            size_t fl;
            const uint8_t *f = byte_frame_rx_frame(&s->decoder, &fl);
            unsigned seq = ((unsigned)f[0] << 8) | f[1];
            uint8_t expect[700];
            size_t el = build_frame(seq, expect);
            CHECK(fl == el && memcmp(f, expect, fl) == 0);
            if (seq < s->next_expected)
            {
                s->out_of_order++;
            }
            s->next_expected = seq + 1;
            s->received++;
        }
    }
}

static void in_memory_with_faults(void)
{
    Side a;
    Side b;
    TestFaults faults_ab = {0, 0, 10, 3, false}; /* corrupt every 10th write, 3-byte reads */
    TestFaults faults_ba = {0, 0, 7, 1, false};  /* corrupt every 7th write, 1-byte reads */
    uint8_t frame[700];
    uint8_t wire[2 * 704 + 2];
    uint8_t chunk[64];

    test_pair_init(&s_pair);
    test_pair_set_faults(&s_pair, 0, &faults_ab);
    test_pair_set_faults(&s_pair, 1, &faults_ba);
    side_init(&a);
    side_init(&b);

    for (unsigned seq = 0; seq < FRAMES_PER_SIDE; seq++)
    {
        size_t fl = build_frame(seq, frame);
        size_t wl = byte_frame_encode_accm(frame, fl, BYTE_FRAME_ACCM_NONE, true, wire, sizeof wire);
        CHECK(wl > 0);
        CHECK_EQ(test_pair_write(&s_pair, 0, wire, wl), (int)wl);
        CHECK_EQ(test_pair_write(&s_pair, 1, wire, wl), (int)wl);

        /* Drain both directions in small chunks, as a real read loop would. */
        size_t n;
        while ((n = test_pair_read(&s_pair, 1, chunk, sizeof chunk)) > 0)
        {
            side_consume(&b, chunk, n);
        }
        while ((n = test_pair_read(&s_pair, 0, chunk, sizeof chunk)) > 0)
        {
            side_consume(&a, chunk, n);
        }
    }

    /* Every corrupted write is an FCS error (or, if the flip hit a flag/escape position, an abort
     * or runt); every other frame arrived, in order. */
    unsigned bad_b = b.decoder.stats.fcs_errors + b.decoder.stats.aborts + b.decoder.stats.runts +
                     b.decoder.stats.too_long;
    unsigned bad_a = a.decoder.stats.fcs_errors + a.decoder.stats.aborts + a.decoder.stats.runts +
                     a.decoder.stats.too_long;
    CHECK_EQ(s_pair.channel[0].corrupted, FRAMES_PER_SIDE / 10);
    CHECK_EQ(s_pair.channel[1].corrupted, FRAMES_PER_SIDE / 7);
    CHECK(bad_b >= s_pair.channel[0].corrupted - 1 && bad_b <= s_pair.channel[0].corrupted + 1);
    CHECK(bad_a >= s_pair.channel[1].corrupted - 1 && bad_a <= s_pair.channel[1].corrupted + 1);
    CHECK(b.received + bad_b >= FRAMES_PER_SIDE - 1);
    CHECK(a.received + bad_a >= FRAMES_PER_SIDE - 1);
    CHECK_EQ(b.out_of_order, 0);
    CHECK_EQ(a.out_of_order, 0);
    printf("  in-memory: a received %u (bad %u), b received %u (bad %u)\n", a.received, bad_a,
           b.received, bad_b);
}

static void in_memory_hold_and_release(void)
{
    /* Bytes written while held are invisible until released; a frame cut in two by the release
     * boundary is still decoded whole. */
    Side b;
    TestFaults hold = {0, 0, 0, 0, true};
    uint8_t frame[700];
    uint8_t wire[2 * 704 + 2];
    uint8_t chunk[2 * 704 + 2];
    test_pair_init(&s_pair);
    test_pair_set_faults(&s_pair, 0, &hold);
    side_init(&b);
    size_t fl = build_frame(5, frame);
    size_t wl = byte_frame_encode_accm(frame, fl, BYTE_FRAME_ACCM_NONE, true, wire, sizeof wire);
    CHECK_EQ(test_pair_write(&s_pair, 0, wire, wl), (int)wl);
    CHECK_EQ(test_pair_pending(&s_pair, 1), 0);
    test_pair_release(&s_pair, 0, wl / 2);
    CHECK_EQ(test_pair_pending(&s_pair, 1), wl / 2);
    side_consume(&b, chunk, test_pair_read(&s_pair, 1, chunk, sizeof chunk));
    CHECK_EQ(b.received, 0);
    test_pair_release(&s_pair, 0, 0);
    side_consume(&b, chunk, test_pair_read(&s_pair, 1, chunk, sizeof chunk));
    CHECK_EQ(b.received, 1);
}

static void over_loopback_tcp(void)
{
    Side a;
    Side b;
    uint8_t frame[700];
    uint8_t wire[2 * 704 + 2];
    uint8_t chunk[512];
    unsigned sent = 0;

    if (test_tcp_pair_open(&s_tcp) != 0)
    {
        CHECK(false);
        return;
    }
    side_init(&a);
    side_init(&b);

    /* Push frames both ways while draining; a non-blocking socket may accept a partial write, so
     * the encoder's resumable form is used here as the real transport pump will. */
    byte_frame_tx enc_a;
    byte_frame_tx enc_b;
    byte_frame_tx_init(&enc_a, BYTE_FRAME_ACCM_NONE, true);
    byte_frame_tx_init(&enc_b, BYTE_FRAME_ACCM_NONE, true);
    uint8_t frame_a[700];
    uint8_t frame_b[700];
    uint32_t deadline = test_now_ms() + 10000;

    while ((a.received < FRAMES_PER_SIDE || b.received < FRAMES_PER_SIDE) &&
           test_now_ms() < deadline)
    {
        if (sent < FRAMES_PER_SIDE && byte_frame_tx_done(&enc_a) && byte_frame_tx_done(&enc_b))
        {
            size_t fl = build_frame(sent, frame_a);
            memcpy(frame_b, frame_a, fl);
            byte_frame_tx_begin(&enc_a, frame_a, fl);
            byte_frame_tx_begin(&enc_b, frame_b, fl);
            sent++;
        }
        if (!byte_frame_tx_done(&enc_a))
        {
            size_t n = byte_frame_tx_produce(&enc_a, wire, 97); /* odd size: splits escapes */
            int w = 0;
            while ((size_t)w < n)
            {
                int k = test_tcp_write(s_tcp.sock[0], wire + w, n - (size_t)w);
                CHECK(k >= 0);
                if (k <= 0)
                {
                    test_sleep_ms(1);
                    continue;
                }
                w += k;
            }
        }
        if (!byte_frame_tx_done(&enc_b))
        {
            size_t n = byte_frame_tx_produce(&enc_b, wire, 131);
            int w = 0;
            while ((size_t)w < n)
            {
                int k = test_tcp_write(s_tcp.sock[1], wire + w, n - (size_t)w);
                CHECK(k >= 0);
                if (k <= 0)
                {
                    test_sleep_ms(1);
                    continue;
                }
                w += k;
            }
        }
        int r;
        while ((r = test_tcp_read(s_tcp.sock[1], chunk, sizeof chunk)) > 0)
        {
            side_consume(&b, chunk, (size_t)r);
        }
        CHECK(r == 0);
        while ((r = test_tcp_read(s_tcp.sock[0], chunk, sizeof chunk)) > 0)
        {
            side_consume(&a, chunk, (size_t)r);
        }
        CHECK(r == 0);
        (void)frame;
    }
    CHECK_EQ(a.received, FRAMES_PER_SIDE);
    CHECK_EQ(b.received, FRAMES_PER_SIDE);
    CHECK_EQ(a.decoder.stats.fcs_errors, 0);
    CHECK_EQ(b.decoder.stats.fcs_errors, 0);
    CHECK_EQ(a.out_of_order, 0);
    CHECK_EQ(b.out_of_order, 0);
    printf("  tcp loopback port %u: %u frames each way\n", (unsigned)s_tcp.port, FRAMES_PER_SIDE);
    test_tcp_pair_close(&s_tcp);
}

int main(void)
{
    printf("[pair_hdlc]\n");
    in_memory_with_faults();
    in_memory_hold_and_release();
    over_loopback_tcp();
    TEST_DONE();
}
