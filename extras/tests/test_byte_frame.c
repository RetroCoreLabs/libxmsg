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

static void encode_escapes(void)
{
    static const uint8_t in[] = { 0x01, 0x7E, 0x41, 0x7D, 0x1F, 0x20 };
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
    memset(&d, 0, sizeof d);
    byte_frame_decoder_reset(&d);
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
    static const uint8_t wire[] = { 0x55, 0x66, 0x7E, 0x7E, 0x7E, 0x01, 0x7D, 0x7E, 0x02, 0x03, 0x7E };
    uint8_t got[BYTE_FRAME_MAX];
    int got_len = 0;
    byte_frame_decoder d;
    memset(&d, 0, sizeof d);
    byte_frame_decoder_reset(&d);
    CHECK_EQ(feed_all(&d, wire, (int)sizeof wire, got, &got_len), 1);
    CHECK_BYTES(got, (size_t)got_len, "02 03");
}

static void overlong_counted_and_survives_reset(void)
{
    uint8_t got[BYTE_FRAME_MAX];
    int got_len = 0;
    byte_frame_decoder d;
    memset(&d, 0, sizeof d);
    byte_frame_decoder_reset(&d);
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

int main(void)
{
    encode_escapes();
    round_trip_all_byte_values();
    noise_empty_and_bad_endings();
    overlong_counted_and_survives_reset();
    TEST_DONE();
}
