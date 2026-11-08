/*
 * test_fcs16.c - the frame check sequence against the captured anchor and the bitwise definition.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/hdlc/fcs16.h"

#include "test_util.h"

/* The SABM from node 100: FCS 0x092E, sent as 2E 09 (HdlcEncoderTests.cs, from a capture). */
static const uint8_t SABM_100[] = {0x01, 0x3F, 0x00, 0x64};
static const uint8_t SABM_100_WITH_FCS[] = {0x01, 0x3F, 0x00, 0x64, 0x2E, 0x09};

static void anchor_vector(void)
{
    uint8_t fcs[2];
    CHECK_EQ(fcs16_compute(SABM_100, sizeof SABM_100), (uint16_t)~0x092Eu);
    fcs16_append(fcs, fcs16_compute(SABM_100, sizeof SABM_100));
    CHECK_BYTES(fcs, 2, "2E 09");
    CHECK(fcs16_is_good(SABM_100_WITH_FCS, sizeof SABM_100_WITH_FCS));
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
        fcs16_append(frame + len, fcs16_compute(frame, len));
        CHECK_EQ(fcs16_compute(frame, len + 2), FCS16_GOOD);
        CHECK(fcs16_is_good(frame, len + 2) || len + 2 < 4);
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
            CHECK(!fcs16_is_good(frame, sizeof frame));
        }
    }
}

static void short_runs_are_never_good(void)
{
    uint8_t two[] = {0x01, 0x02};
    uint8_t three[] = {0x01, 0x02, 0x03};
    CHECK(!fcs16_is_good(two, 2));
    CHECK(!fcs16_is_good(three, 3));
    CHECK(!fcs16_is_good(two, 0));
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
        /* fcs16_update from register 0 over one byte equals table[byte]. */
        uint8_t byte = (uint8_t)i;
        CHECK_EQ(fcs16_update(0, &byte, 1), c);
    }
}

static void update_is_incremental(void)
{
    uint8_t data[300];
    for (size_t i = 0; i < sizeof data; i++)
    {
        data[i] = (uint8_t)(i ^ 0xA5);
    }
    uint16_t whole = fcs16_compute(data, sizeof data);
    uint16_t parts = fcs16_update(fcs16_update(FCS16_INIT, data, 100), data + 100, 200);
    CHECK_EQ(parts, whole);
    CHECK_EQ(fcs16_update(FCS16_INIT, NULL, 0), FCS16_INIT);
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
