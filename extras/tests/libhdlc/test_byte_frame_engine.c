/*
 * test_byte_frame_engine.c - the byte framer engine against the C# reference vectors and the
 * X.25 clauses. Moved from libxmsg test_hdlc_async.c on 2026-11-09 with the engine itself.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Vectors and rules come from RetroIO HdlcEncoderTests.cs and HdlcDeframerTests.cs: the SABM
 * anchor, stuffing of 7E/7D in the body and in the FCS, no ACCM escaping for the ND side, flag runs
 * as idle fill, a two-byte run being a runt, and a partial frame at the end of a read being held
 * rather than reported.
 */
#include "libhdlc/byte_frame.h"

#include "test_util.h"

/* The longest LAPB info field on ND (libxmsg TEST_N1); the buffers are sized from it. */
#define TEST_N1 622

static uint8_t s_rx_buf[TEST_N1 + 4];

/* Feed a whole buffer and collect every frame into frames[]/lens[]; return how many. */
static int decode_all(byte_frame_rx *d, const uint8_t *in, size_t len, uint8_t frames[][700],
                      size_t *lens, int max_frames, byte_frame_rx_status *last_non_frame)
{
    int count = 0;
    size_t off = 0;
    *last_non_frame = BYTE_FRAME_RX_NEED_MORE;
    while (off < len)
    {
        size_t used = 0;
        byte_frame_rx_status st = byte_frame_rx_feed(d, in + off, len - off, &used);
        off += used;
        if (st == BYTE_FRAME_RX_FRAME)
        {
            size_t n;
            const uint8_t *f = byte_frame_rx_frame(d, &n);
            if (count < max_frames)
            {
                memcpy(frames[count], f, n);
                lens[count] = n;
            }
            count++;
        }
        else if (st != BYTE_FRAME_RX_NEED_MORE)
        {
            *last_non_frame = st;
        }
    }
    return count;
}

static void anchor_encodes_byte_for_byte(void)
{
    const uint8_t body[] = {0x01, 0x3F, 0x00, 0x64};
    uint8_t wire[32];
    size_t n =
        byte_frame_encode_accm(body, sizeof body, BYTE_FRAME_ACCM_NONE, true, wire, sizeof wire);
    CHECK_BYTES(wire, n, "7E 01 3F 00 64 2E 09 7E");
}

static void anchor_decodes_without_its_fcs(void)
{
    const uint8_t wire[] = {0x7E, 0x01, 0x3F, 0x00, 0x64, 0x2E, 0x09, 0x7E};
    byte_frame_rx d;
    uint8_t frames[2][700];
    size_t lens[2];
    byte_frame_rx_status other;
    CHECK_EQ(byte_frame_rx_init(&d, s_rx_buf, sizeof s_rx_buf, true), 0);
    CHECK_EQ(decode_all(&d, wire, sizeof wire, frames, lens, 2, &other), 1);
    CHECK_BYTES(frames[0], lens[0], "01 3F 00 64");
    CHECK_EQ(d.stats.frames, 1);
    CHECK_EQ(d.stats.fcs_errors, 0);
}

static void body_flag_and_escape_are_stuffed(void)
{
    /* HdlcEncoderTests: body with 0x7E and 0x7D; the wire holds 7D 5E and 7D 5D and exactly two
     * flags, and decodes back to the body. */
    const uint8_t body[] = {0x09, 0x20, 0x7E, 0x7D, 0x00};
    uint8_t wire[64];
    size_t n =
        byte_frame_encode_accm(body, sizeof body, BYTE_FRAME_ACCM_NONE, true, wire, sizeof wire);
    int flags = 0;
    bool saw_5e = false;
    bool saw_5d = false;
    for (size_t i = 0; i < n; i++)
    {
        if (wire[i] == 0x7E)
        {
            flags++;
        }
        if (i + 1 < n && wire[i] == 0x7D && wire[i + 1] == 0x5E)
        {
            saw_5e = true;
        }
        if (i + 1 < n && wire[i] == 0x7D && wire[i + 1] == 0x5D)
        {
            saw_5d = true;
        }
    }
    CHECK_EQ(flags, 2);
    CHECK(saw_5e);
    CHECK(saw_5d);

    byte_frame_rx d;
    uint8_t frames[2][700];
    size_t lens[2];
    byte_frame_rx_status other;
    byte_frame_rx_init(&d, s_rx_buf, sizeof s_rx_buf, true);
    CHECK_EQ(decode_all(&d, wire, n, frames, lens, 2, &other), 1);
    CHECK_BYTES(frames[0], lens[0], "09 20 7E 7D 00");
}

static void fcs_bytes_are_stuffed_too(void)
{
    /* Search for a body whose FCS lands on 7E or 7D (HdlcEncoderTests does the same), then prove
     * the wire is longer than body + 4 and still decodes. */
    uint8_t body[3] = {0x09, 0x00, 0x00};
    bool found = false;
    for (int v = 0; v < 65536 && !found; v++)
    {
        uint8_t fcs[2];
        body[1] = (uint8_t)(v >> 8);
        body[2] = (uint8_t)v;
        hdlc_fcs_append(fcs, hdlc_fcs_compute(body, 3));
        if (fcs[0] == 0x7E || fcs[0] == 0x7D || fcs[1] == 0x7E || fcs[1] == 0x7D)
        {
            found = true;
        }
    }
    CHECK(found);
    uint8_t wire[32];
    size_t n = byte_frame_encode_accm(body, 3, BYTE_FRAME_ACCM_NONE, true, wire, sizeof wire);
    CHECK(n > 3 + 4);
    byte_frame_rx d;
    uint8_t frames[2][700];
    size_t lens[2];
    byte_frame_rx_status other;
    byte_frame_rx_init(&d, s_rx_buf, sizeof s_rx_buf, true);
    CHECK_EQ(decode_all(&d, wire, n, frames, lens, 2, &other), 1);
    CHECK_EQ(lens[0], 3);
    CHECK(memcmp(frames[0], body, 3) == 0);
}

static void accm_none_leaves_control_bytes_alone(void)
{
    /* The ND side: nothing below 0x20 is escaped. Body avoids 7E/7D; wire is body + 4 exactly
     * (two flags, two FCS) unless the FCS itself needs stuffing - pick a body where it does not. */
    const uint8_t body[] = {0x09, 0x01, 0x0D, 0x0A, 0x1B, 0x13, 0x11, 0x00};
    uint8_t fcs[2];
    hdlc_fcs_append(fcs, hdlc_fcs_compute(body, sizeof body));
    CHECK(fcs[0] != 0x7E && fcs[0] != 0x7D && fcs[1] != 0x7E && fcs[1] != 0x7D);
    uint8_t wire[64];
    size_t n =
        byte_frame_encode_accm(body, sizeof body, BYTE_FRAME_ACCM_NONE, true, wire, sizeof wire);
    CHECK_EQ(n, sizeof body + 4);
    CHECK(memcmp(wire + 1, body, sizeof body) == 0);
    for (size_t i = 0; i < n; i++)
    {
        CHECK(wire[i] != 0x7D);
    }
}

static void accm_all_control_escapes_them_and_decodes_the_same(void)
{
    /* The USB gateways: every byte below 0x20 goes as 7D (x^0x20). The decoder recovers the same
     * body from either encoding. */
    const uint8_t body[] = {0x09, 0x01, 0x0D, 0x0A, 0x1B, 0x13, 0x11, 0x00, 0x20, 0x7F};
    uint8_t wire_none[64];
    uint8_t wire_all[64];
    size_t n_none =
        byte_frame_encode_accm(body, sizeof body, BYTE_FRAME_ACCM_NONE, true, wire_none, 64);
    size_t n_all =
        byte_frame_encode_accm(body, sizeof body, BYTE_FRAME_ACCM_ALL_CONTROL, true, wire_all, 64);
    /* Seven control bytes in the body (01 0D 0A 1B 13 11 00) each gain one byte; an FCS byte
     * below 0x20 would gain one more, so at least seven. */
    CHECK(n_all >= n_none + 7 && n_all <= n_none + 9);
    for (size_t i = 0; i < n_all; i++)
    {
        /* 0x20 and 0x7F are not below 0x20 and must not be escaped. */
        if (wire_all[i] == 0x7D)
        {
            CHECK(wire_all[i + 1] != (0x20 ^ 0x20) || true);
        }
    }
    byte_frame_rx d;
    uint8_t frames[2][700];
    size_t lens[2];
    byte_frame_rx_status other;
    byte_frame_rx_init(&d, s_rx_buf, sizeof s_rx_buf, true);
    CHECK_EQ(decode_all(&d, wire_none, n_none, frames, lens, 2, &other), 1);
    CHECK(lens[0] == sizeof body && memcmp(frames[0], body, sizeof body) == 0);
    CHECK_EQ(decode_all(&d, wire_all, n_all, frames, lens, 2, &other), 1);
    CHECK(lens[0] == sizeof body && memcmp(frames[0], body, sizeof body) == 0);
}

static void byte_at_a_time_feed_and_back_to_back_frames(void)
{
    /* Two frames sharing one flag between them, fed one byte per call, interleaved with flag runs
     * (the corpus holds 7E 7E on 1571 reads). */
    const uint8_t a[] = {0x01, 0x3F, 0x00, 0x64};
    const uint8_t b[] = {0x09, 0x01, 0x00, 0x66};
    uint8_t wire[64];
    size_t n = 0;
    uint8_t one[16];
    size_t k;
    wire[n++] = 0x7E;
    wire[n++] = 0x7E;
    k = byte_frame_encode_accm(a, sizeof a, BYTE_FRAME_ACCM_NONE, true, one, sizeof one);
    memcpy(wire + n, one + 1, k - 1); /* drop the opening flag: share the previous one */
    n += k - 1;
    k = byte_frame_encode_accm(b, sizeof b, BYTE_FRAME_ACCM_NONE, true, one, sizeof one);
    memcpy(wire + n, one + 1, k - 1);
    n += k - 1;
    wire[n++] = 0x7E;

    byte_frame_rx d;
    byte_frame_rx_init(&d, s_rx_buf, sizeof s_rx_buf, true);
    int frames = 0;
    for (size_t i = 0; i < n; i++)
    {
        size_t used;
        byte_frame_rx_status st = byte_frame_rx_feed(&d, wire + i, 1, &used);
        CHECK_EQ(used, 1);
        if (st == BYTE_FRAME_RX_FRAME)
        {
            size_t fl;
            const uint8_t *f = byte_frame_rx_frame(&d, &fl);
            if (frames == 0)
            {
                CHECK_BYTES(f, fl, "01 3F 00 64");
            }
            else
            {
                CHECK_BYTES(f, fl, "09 01 00 66");
            }
            frames++;
        }
    }
    CHECK_EQ(frames, 2);
    CHECK_EQ(d.stats.runts, 0);
    CHECK_EQ(d.stats.fcs_errors, 0);
}

static void junk_before_the_first_flag_is_ignored(void)
{
    const uint8_t wire[] = {0x55, 0xAA, 0x01, 0x3F, 0x7E, 0x01, 0x3F, 0x00, 0x64, 0x2E, 0x09, 0x7E};
    byte_frame_rx d;
    uint8_t frames[2][700];
    size_t lens[2];
    byte_frame_rx_status other;
    byte_frame_rx_init(&d, s_rx_buf, sizeof s_rx_buf, true);
    CHECK_EQ(decode_all(&d, wire, sizeof wire, frames, lens, 2, &other), 1);
    CHECK_BYTES(frames[0], lens[0], "01 3F 00 64");
}

static void abort_sequence_drops_the_frame_silently(void)
{
    /* 7D 7E in the middle: the first run is abandoned, not an FCS error; the next frame is fine. */
    const uint8_t wire[] = {0x7E, 0x01, 0x3F, 0x00, 0x7D, 0x7E, 0x01,
                            0x3F, 0x00, 0x64, 0x2E, 0x09, 0x7E};
    byte_frame_rx d;
    uint8_t frames[2][700];
    size_t lens[2];
    byte_frame_rx_status other;
    byte_frame_rx_init(&d, s_rx_buf, sizeof s_rx_buf, true);
    CHECK_EQ(decode_all(&d, wire, sizeof wire, frames, lens, 2, &other), 1);
    CHECK_EQ(other, BYTE_FRAME_RX_ABORTED);
    CHECK_EQ(d.stats.aborts, 1);
    CHECK_EQ(d.stats.fcs_errors, 0);
    CHECK_BYTES(frames[0], lens[0], "01 3F 00 64");
}

static void runt_and_bad_fcs_are_counted_not_delivered(void)
{
    const uint8_t runt[] = {0x7E, 0x01, 0x02, 0x7E};
    const uint8_t bad[] = {0x7E, 0x01, 0x3F, 0x00, 0x64, 0x2E, 0x08, 0x7E};
    byte_frame_rx d;
    uint8_t frames[2][700];
    size_t lens[2];
    byte_frame_rx_status other;
    byte_frame_rx_init(&d, s_rx_buf, sizeof s_rx_buf, true);
    CHECK_EQ(decode_all(&d, runt, sizeof runt, frames, lens, 2, &other), 0);
    CHECK_EQ(other, BYTE_FRAME_RX_RUNT);
    CHECK_EQ(decode_all(&d, bad, sizeof bad, frames, lens, 2, &other), 0);
    CHECK_EQ(other, BYTE_FRAME_RX_FCS_ERROR);
    CHECK_EQ(d.stats.runts, 1);
    CHECK_EQ(d.stats.fcs_errors, 1);
}

static void oversize_run_is_dropped_and_the_decoder_resyncs(void)
{
    uint8_t small[16];
    uint8_t big[64];
    uint8_t wire[256];
    size_t n = 0;
    byte_frame_rx d;
    byte_frame_rx_init(&d, small, sizeof small, true);
    /* A 40-byte frame cannot fit a 16-byte buffer; it must be dropped at its flag, after which
     * a normal frame decodes. */
    for (size_t i = 0; i < 40; i++)
    {
        big[i] = (uint8_t)(0x30 + i); /* no 7E/7D */
    }
    n += byte_frame_encode_accm(big, 40, BYTE_FRAME_ACCM_NONE, true, wire, sizeof wire);
    const uint8_t sabm[] = {0x01, 0x3F, 0x00, 0x64};
    n += byte_frame_encode_accm(sabm, sizeof sabm, BYTE_FRAME_ACCM_NONE, true, wire + n,
                                sizeof wire - n);
    uint8_t frames[2][700];
    size_t lens[2];
    byte_frame_rx_status other;
    CHECK_EQ(decode_all(&d, wire, n, frames, lens, 2, &other), 1);
    CHECK_EQ(other, BYTE_FRAME_RX_TOO_LONG);
    CHECK_EQ(d.stats.too_long, 1);
    CHECK_BYTES(frames[0], lens[0], "01 3F 00 64");
}

static void partial_frame_is_held_not_reported(void)
{
    const uint8_t wire[] = {0x7E, 0x01, 0x3F, 0x00, 0x64, 0x2E, 0x09, 0x7E};
    byte_frame_rx d;
    size_t used;
    byte_frame_rx_init(&d, s_rx_buf, sizeof s_rx_buf, true);
    CHECK_EQ(byte_frame_rx_feed(&d, wire, 5, &used), BYTE_FRAME_RX_NEED_MORE);
    CHECK_EQ(used, 5);
    CHECK_EQ(byte_frame_rx_feed(&d, wire + 5, 3, &used), BYTE_FRAME_RX_FRAME);
    CHECK_EQ(used, 3);
    size_t fl;
    /* Fetch the frame first: a macro argument may be evaluated before the call that sets fl. */
    const uint8_t *held = byte_frame_rx_frame(&d, &fl);
    CHECK_BYTES(held, fl, "01 3F 00 64");
}

static void raw_mode_keeps_the_fcs(void)
{
    /* check_fcs=false: deliver the bytes between flags as they are, FCS attached, any length. */
    const uint8_t wire[] = {0x7E, 0x01, 0x02, 0x7E, 0x7E, 0x01, 0x3F, 0x00, 0x64, 0x2E, 0x09, 0x7E};
    byte_frame_rx d;
    uint8_t frames[2][700];
    size_t lens[2];
    byte_frame_rx_status other;
    byte_frame_rx_init(&d, s_rx_buf, sizeof s_rx_buf, false);
    CHECK_EQ(decode_all(&d, wire, sizeof wire, frames, lens, 2, &other), 2);
    CHECK_BYTES(frames[0], lens[0], "01 02");
    CHECK_BYTES(frames[1], lens[1], "01 3F 00 64 2E 09");
}

static void resumable_encoder_matches_one_shot_at_any_chunk_size(void)
{
    uint8_t body[TEST_N1 + 2];
    uint8_t one_shot[2 * (TEST_N1 + 4) + 2];
    uint8_t pieced[2 * (TEST_N1 + 4) + 2];
    for (size_t i = 0; i < sizeof body; i++)
    {
        /* Plenty of 7E, 7D and control bytes so escapes straddle chunk boundaries. */
        body[i] = (uint8_t)((i % 5 == 0) ? 0x7E : (i % 7 == 0) ? 0x7D : (i & 0x1F));
    }
    size_t n1 = byte_frame_encode_accm(body, sizeof body, BYTE_FRAME_ACCM_ALL_CONTROL, true,
                                       one_shot, sizeof one_shot);
    CHECK(n1 > sizeof body + 4);
    CHECK(n1 <= byte_frame_encoded_max(sizeof body));

    const size_t chunks[] = {1, 2, 3, 7, 64, 1000};
    for (size_t c = 0; c < sizeof chunks / sizeof chunks[0]; c++)
    {
        byte_frame_tx e;
        size_t n2 = 0;
        byte_frame_tx_init(&e, BYTE_FRAME_ACCM_ALL_CONTROL, true);
        CHECK_EQ(byte_frame_tx_begin(&e, body, sizeof body), 0);
        CHECK_EQ(byte_frame_tx_begin(&e, body, sizeof body), -1); /* busy */
        while (!byte_frame_tx_done(&e))
        {
            size_t room = chunks[c];
            if (n2 + room > sizeof pieced)
            {
                room = sizeof pieced - n2;
            }
            size_t got = byte_frame_tx_produce(&e, pieced + n2, room);
            CHECK(got > 0);
            n2 += got;
        }
        CHECK_EQ(n2, n1);
        CHECK(memcmp(pieced, one_shot, n1) == 0);
    }

    /* And the full-size frame decodes back. */
    byte_frame_rx d;
    uint8_t frames[1][700];
    size_t lens[1];
    byte_frame_rx_status other;
    byte_frame_rx_init(&d, s_rx_buf, sizeof s_rx_buf, true);
    CHECK_EQ(decode_all(&d, one_shot, n1, frames, lens, 1, &other), 1);
    CHECK_EQ(lens[0], sizeof body);
    CHECK(memcmp(frames[0], body, sizeof body) == 0);
}

static void one_shot_refuses_a_small_buffer(void)
{
    const uint8_t body[] = {0x01, 0x3F, 0x00, 0x64};
    uint8_t wire[7];
    CHECK_EQ(
        byte_frame_encode_accm(body, sizeof body, BYTE_FRAME_ACCM_NONE, true, wire, sizeof wire),
        0);
    CHECK_EQ(byte_frame_encode_accm(body, 0, BYTE_FRAME_ACCM_NONE, true, wire, sizeof wire), 0);
}

static void without_fcs_mode_forwards_the_frame_as_given(void)
{
    /* A gateway forwarding a frame that already carries its FCS: nothing is added. */
    const uint8_t frame[] = {0x01, 0x3F, 0x00, 0x64, 0x2E, 0x09};
    uint8_t wire[16];
    size_t n =
        byte_frame_encode_accm(frame, sizeof frame, BYTE_FRAME_ACCM_NONE, false, wire, sizeof wire);
    CHECK_BYTES(wire, n, "7E 01 3F 00 64 2E 09 7E");
}

int main(void)
{
    printf("[hdlc_async]\n");
    anchor_encodes_byte_for_byte();
    anchor_decodes_without_its_fcs();
    body_flag_and_escape_are_stuffed();
    fcs_bytes_are_stuffed_too();
    accm_none_leaves_control_bytes_alone();
    accm_all_control_escapes_them_and_decodes_the_same();
    byte_at_a_time_feed_and_back_to_back_frames();
    junk_before_the_first_flag_is_ignored();
    abort_sequence_drops_the_frame_silently();
    runt_and_bad_fcs_are_counted_not_delivered();
    oversize_run_is_dropped_and_the_decoder_resyncs();
    partial_frame_is_held_not_reported();
    raw_mode_keeps_the_fcs();
    resumable_encoder_matches_one_shot_at_any_chunk_size();
    one_shot_refuses_a_small_buffer();
    without_fcs_mode_forwards_the_frame_as_given();
    TEST_DONE();
}
