/*
 * test_fcs.c - the frame check sequence against the captured anchor and the bitwise definition.
 * Moved from libxmsg test_fcs16.c on 2026-11-09.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libhdlc/hdlc.h"

#include "test_util.h"

/* The SABM from node 100: FCS 0x092E, sent as 2E 09 (HdlcEncoderTests.cs, from a capture). */
static const uint8_t SABM_100[] = {0x01, 0x3F, 0x00, 0x64};
static const uint8_t SABM_100_WITH_FCS[] = {0x01, 0x3F, 0x00, 0x64, 0x2E, 0x09};

static void anchor_vector(void)
{
    uint8_t fcs[2];
    CHECK_EQ(hdlc_fcs_compute(SABM_100, sizeof SABM_100), (uint16_t)~0x092Eu);
    hdlc_fcs_append(fcs, hdlc_fcs_compute(SABM_100, sizeof SABM_100));
    CHECK_BYTES(fcs, 2, "2E 09");
    CHECK(hdlc_fcs_is_good(SABM_100_WITH_FCS, sizeof SABM_100_WITH_FCS));
}

static void residue_rule(void)
{
    /* Any frame followed by its own FCS folds to the good residue. */
    uint8_t frame[700];
    for (size_t len = 1; len <= 600; len += 37)
    {
        for (size_t i = 0; i < len; i++)
        {
            frame[i] = (uint8_t)(i * 7 + len);
        }
        hdlc_fcs_append(frame + len, hdlc_fcs_compute(frame, len));
        CHECK_EQ(hdlc_fcs_compute(frame, len + 2), HDLC_CRC_GOOD);
        CHECK(hdlc_fcs_is_good(frame, len + 2) || len + 2 < 4);
    }
}

static void corruption_is_caught(void)
{
    uint8_t frame[sizeof SABM_100_WITH_FCS];
    for (size_t at = 0; at < sizeof frame; at++)
    {
        for (int bit = 0; bit < 8; bit++)
        {
            memcpy(frame, SABM_100_WITH_FCS, sizeof frame);
            frame[at] ^= (uint8_t)(1u << bit);
            CHECK(!hdlc_fcs_is_good(frame, sizeof frame));
        }
    }
}

static void short_runs_are_never_good(void)
{
    uint8_t two[] = {0x01, 0x02};
    uint8_t three[] = {0x01, 0x02, 0x03};
    CHECK(!hdlc_fcs_is_good(two, 2));
    CHECK(!hdlc_fcs_is_good(three, 3));
    CHECK(!hdlc_fcs_is_good(two, 0));
}

static void table_matches_bitwise_definition(void)
{
    /* Recompute every table entry from the polynomial and compare through the public API. */
    for (int i = 0; i < 256; i++)
    {
        uint16_t c = (uint16_t)i;
        for (int b = 0; b < 8; b++)
        {
            c = (uint16_t)((c & 1u) ? ((c >> 1) ^ 0x8408u) : (c >> 1));
        }
        /* hdlc_fcs_update from register 0 over one byte equals table[byte]. */
        uint8_t byte = (uint8_t)i;
        CHECK_EQ(hdlc_fcs_update(0, &byte, 1), c);
    }
}

static void update_is_incremental(void)
{
    uint8_t data[300];
    for (size_t i = 0; i < sizeof data; i++)
    {
        data[i] = (uint8_t)(i ^ 0xA5);
    }
    uint16_t whole = hdlc_fcs_compute(data, sizeof data);
    uint16_t parts = hdlc_fcs_update(hdlc_fcs_update(HDLC_CRC_INIT, data, 100), data + 100, 200);
    CHECK_EQ(parts, whole);
    CHECK_EQ(hdlc_fcs_update(HDLC_CRC_INIT, NULL, 0), HDLC_CRC_INIT);
}

int main(void)
{
    printf("[fcs16]\n");
    anchor_vector();
    residue_rule();
    corruption_is_caught();
    short_runs_are_never_good();
    table_matches_bitwise_definition();
    update_is_incremental();
    TEST_DONE();
}
