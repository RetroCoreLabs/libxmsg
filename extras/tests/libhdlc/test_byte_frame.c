/*
 * test_byte_frame.c - RFC 1662 byte stuffing between a gateway and its host.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Expected bytes follow RFC 1662 section 4.2 as this library applies it: 0x7E and 0x7D are
 * escaped, and so is every byte below 0x20 (the full async control character map).
 */
#include "libhdlc/byte_frame.h"
#include "test_util.h"

#include <string.h>

static void encode_escapes(void)
{
    static const uint8_t in[] = {0x01, 0x7E, 0x41, 0x7D, 0x1F, 0x20};
    uint8_t out[2 + 2 * sizeof in];
    int n = byte_frame_encode(in, (int)sizeof in, out);
    CHECK_BYTES(out, (size_t)n, "7E 7D 21 7D 5E 41 7D 5D 7D 3F 20 7E");
}

static int feed_all(byte_frame_decoder *d, const uint8_t *p, int n, uint8_t *got, int *got_len)
{
    int frames = 0;
    for (int i = 0; i < n; i++)
    {
        if (byte_frame_decoder_feed(d, p[i]))
        {
            memcpy(got, d->buffer, (size_t)d->length);
            *got_len = d->length;
            frames++;
        }
    }
    return frames;
}

/* The decoder's buffer: the caller's since 2026-11-16 (byte_frame_decoder_init). */
static uint8_t s_buf[BYTE_FRAME_MAX];

static void round_trip_all_byte_values(void)
{
    uint8_t in[256];
    uint8_t wire[2 + 2 * 256];
    uint8_t got[BYTE_FRAME_MAX];
    int got_len = 0;
    byte_frame_decoder d;
    for (int i = 0; i < 256; i++)
    {
        in[i] = (uint8_t)i;
    }
    CHECK_EQ(byte_frame_decoder_init(&d, s_buf, sizeof s_buf), 0);
    int n = byte_frame_encode(in, 256, wire);
    CHECK_EQ(feed_all(&d, wire, n, got, &got_len), 1);
    CHECK_EQ(got_len, 256);
    CHECK(memcmp(got, in, 256) == 0);
    CHECK(byte_frame_decoder_is_idle(&d));
}

static void noise_empty_and_bad_endings(void)
{
    /* Noise before the first flag, two empty frames, a frame ending on an escape (dropped),
     * then a good one. */
    static const uint8_t wire[] = {0x55, 0x66, 0x7E, 0x7E, 0x7E, 0x01,
                                   0x7D, 0x7E, 0x02, 0x03, 0x7E};
    uint8_t got[BYTE_FRAME_MAX];
    int got_len = 0;
    byte_frame_decoder d;
    CHECK_EQ(byte_frame_decoder_init(&d, s_buf, sizeof s_buf), 0);
    CHECK_EQ(feed_all(&d, wire, (int)sizeof wire, got, &got_len), 1);
    CHECK_BYTES(got, (size_t)got_len, "02 03");
}

static void overlong_counted_and_survives_reset(void)
{
    uint8_t got[BYTE_FRAME_MAX];
    int got_len = 0;
    byte_frame_decoder d;
    CHECK_EQ(byte_frame_decoder_init(&d, s_buf, sizeof s_buf), 0);
    (void)byte_frame_decoder_feed(&d, 0x7E);
    for (int i = 0; i < BYTE_FRAME_MAX + 5; i++)
    {
        CHECK(!byte_frame_decoder_feed(&d, 0x41));
    }
    CHECK(!byte_frame_decoder_feed(&d, 0x7E));
    CHECK_EQ(d.overlong, 1);
    byte_frame_decoder_reset(&d);
    CHECK_EQ(d.overlong, 1);

    /* Exactly BYTE_FRAME_MAX bytes is still a frame. */
    (void)byte_frame_decoder_feed(&d, 0x7E);
    for (int i = 0; i < BYTE_FRAME_MAX; i++)
    {
        (void)byte_frame_decoder_feed(&d, 0x41);
    }
    {
        static const uint8_t flag = 0x7E;
        CHECK_EQ(feed_all(&d, &flag, 1, got, &got_len), 1);
        CHECK_EQ(got_len, BYTE_FRAME_MAX);
    }
}

/*
 * The one behaviour that changed when the short form moved onto the shared engine (2026-11-09):
 * the byte after ANY 0x7D is XORed with 0x20, a second 0x7D included (RFC 1662 4.2). The old
 * decoder stayed in escape state on 7D 7D and XORed the byte after that instead. No correct
 * sender ever writes 7D 7D (a 7D is sent as 7D 5D), so only broken input sees the difference.
 */
static void escape_after_escape(void)
{
    static const uint8_t wire[] = {0x7E, 0x01, 0x7D, 0x7D, 0x41, 0x02, 0x7E};
    byte_frame_decoder d;
    uint8_t got[16];
    int got_len = 0;

    CHECK_EQ(byte_frame_decoder_init(&d, s_buf, sizeof s_buf), 0);
    CHECK_EQ(feed_all(&d, wire, (int)sizeof wire, got, &got_len), 1);
    CHECK_BYTES(got, got_len, "01 5D 41 02");
}

/* Since 2026-11-16 a decoder never given a buffer keeps nothing and counts every byte, so a
 * missing byte_frame_decoder_init() shows in no_buffer instead of passing as a quiet line. */
static void decoder_without_buffer_counts(void)
{
    static byte_frame_decoder d;
    static const uint8_t wire[] = {0x7E, 0x11, 0x22, 0x7E};
    uint8_t got[16];
    int got_len = 0;

    CHECK_EQ(feed_all(&d, wire, (int)sizeof wire, got, &got_len), 0);
    CHECK_EQ(d.no_buffer, 4);
    byte_frame_decoder_reset(&d);
    CHECK_EQ(d.no_buffer, 4);
}

static void init_rejects_bad_arguments(void)
{
    byte_frame_decoder d;
    uint8_t small[8];
    CHECK_EQ(byte_frame_decoder_init(NULL, small, sizeof small), -1);
    CHECK_EQ(byte_frame_decoder_init(&d, NULL, sizeof small), -1);
    CHECK_EQ(byte_frame_decoder_init(&d, small, 7), -1);
    CHECK_EQ(byte_frame_decoder_init(&d, small, sizeof small), 0);
}

/* A caller's smaller buffer is the limit: 16 bytes fit, 17 are dropped and counted. */
static void small_caller_buffer_is_the_limit(void)
{
    static uint8_t small[16];
    uint8_t got[32];
    int got_len = 0;
    byte_frame_decoder d;
    static const uint8_t flag = 0x7E;

    CHECK_EQ(byte_frame_decoder_init(&d, small, sizeof small), 0);
    (void)byte_frame_decoder_feed(&d, 0x7E);
    for (int i = 0; i < 17; i++)
    {
        (void)byte_frame_decoder_feed(&d, 0x42);
    }
    CHECK_EQ(feed_all(&d, &flag, 1, got, &got_len), 0);
    CHECK_EQ(d.overlong, 1);
    for (int i = 0; i < 16; i++)
    {
        (void)byte_frame_decoder_feed(&d, 0x43);
    }
    CHECK_EQ(feed_all(&d, &flag, 1, got, &got_len), 1);
    CHECK_EQ(got_len, 16);
    CHECK(d.buffer == small);
}

/* The sizes HDLCGW will try on the bench (2026-11-14): 1025, 2048, 4103 and 4105 bytes, every
 * byte value, through the encoder and back. */
static void x25_sizes_round_trip(void)
{
    static const int sizes[] = {1025, 2048, 4103, 4105};
    static uint8_t in[4105];
    static uint8_t wire[2 + (2 * 4105)];
    static uint8_t got[BYTE_FRAME_MAX];
    byte_frame_decoder d;

    for (int i = 0; i < 4105; i++)
    {
        in[i] = (uint8_t)((i * 7) + 0x7C);
    }
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++)
    {
        int got_len = 0;
        int n;
        CHECK_EQ(byte_frame_decoder_init(&d, s_buf, sizeof s_buf), 0);
        n = byte_frame_encode(in, sizes[k], wire);
        CHECK_EQ(feed_all(&d, wire, n, got, &got_len), 1);
        CHECK_EQ(got_len, sizes[k]);
        CHECK(memcmp(got, in, (size_t)sizes[k]) == 0);
        CHECK_EQ(d.overlong, 0);
    }
}

int main(void)
{
    encode_escapes();
    round_trip_all_byte_values();
    noise_empty_and_bad_endings();
    overlong_counted_and_survives_reset();
    escape_after_escape();
    decoder_without_buffer_counts();
    init_rejects_bad_arguments();
    small_caller_buffer_is_the_limit();
    x25_sizes_round_trip();
    TEST_DONE();
}
