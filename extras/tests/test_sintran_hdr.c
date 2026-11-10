/*
 * test_sintran_hdr.c - the SINTRAN header checksum against four captured headers.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * The four vectors: the D100 accept the registry README cites (checksum D9F3), the reachability
 * request 100 -> 103 whose fabricated checksum DE00 once crashed D100 (correct DE07), the worked
 * example in the registry (DE08), and the announce from LapbAnnounceOrderingTests (904F).
 */
#include <string.h>
#include "libxmsg/sintran/sintran_hdr.h"

#include "test_util.h"

static void captured_checksums_reproduce(void)
{
    uint8_t h[14];
    CHECK_EQ(test_hex_to_bytes("21 13 00 0E 00 66 00 64 00 21 04 00 D9 F3", h, 14), 14);
    CHECK_EQ(sintran_hdr_checksum(h), 0xD9F3);
    CHECK_EQ(test_hex_to_bytes("21 13 00 19 00 64 00 67 FF FF 00 01 DE 07", h, 14), 14);
    CHECK_EQ(sintran_hdr_checksum(h), 0xDE07);
    CHECK_EQ(test_hex_to_bytes("21 13 00 19 00 66 00 64 FF FF 00 01 DE 08", h, 14), 14);
    CHECK_EQ(sintran_hdr_checksum(h), 0xDE08);
    CHECK_EQ(test_hex_to_bytes("21 13 00 19 4E 1F 00 64 FF FF 00 01 90 4F", h, 14), 14);
    CHECK_EQ(sintran_hdr_checksum(h), 0x904F);
}

static void parse_accepts_good_and_rejects_bad(void)
{
    uint8_t h[14];
    SintranHeader s;
    test_hex_to_bytes("21 13 00 0E 00 66 00 64 00 21 04 00 D9 F3", h, 14);
    CHECK_EQ(sintran_hdr_parse(h, 14, &s), XMSG_OK);
    CHECK_EQ(s.version, 0x21);
    CHECK_EQ(s.hop, 0x13);
    CHECK_EQ(s.subtype, SINTRAN_SUBTYPE_DATA);
    CHECK_EQ(s.dst_node, 102);
    CHECK_EQ(s.src_node, 100);
    CHECK_EQ(s.flags1, 0x0021);
    CHECK_EQ(s.flags2, 0x0400);
    CHECK_EQ(s.checksum, 0xD9F3);
    h[13] ^= 0x01;
    CHECK_EQ(sintran_hdr_parse(h, 14, &s), XMSG_EPROTO);
    h[13] ^= 0x01;
    h[0] = 0x22;
    CHECK_EQ(sintran_hdr_parse(h, 14, &s), XMSG_EPROTO);
    h[0] = 0x21;
    CHECK_EQ(sintran_hdr_parse(h, 13, &s), XMSG_EINVAL);
}

static void write_fills_the_checksum(void)
{
    SintranHeader s;
    uint8_t out[14];
    sintran_hdr_fill(&s, SINTRAN_SUBTYPE_REACH_REQUEST, 100, 103, 0xFFFF, 0x0001);
    sintran_hdr_write(out, &s);
    CHECK_BYTES(out, 14, "21 13 00 19 00 64 00 67 FF FF 00 01 DE 07");
    CHECK_EQ(s.checksum, 0xDE07);
}

static void zero_sum_stays_zero(void)
{
    /* The kernel skips the complement when the sum is exactly zero. Six zero words sum to zero. */
    uint8_t h[14] = {0};
    CHECK_EQ(sintran_hdr_checksum(h), 0);
}

static void end_around_carry_is_folded(void)
{
    /* FFFF + 0001 = 1 0000 -> folds to 0001; plus 0 ... ; complement FFFE. */
    uint8_t h[14] = {0xFF, 0xFF, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK_EQ(sintran_hdr_checksum(h), 0xFFFE);
}

static void error_names(void)
{
    CHECK(strcmp(sintran_error_name(XMSG_XE_XERNA), "XERNA Remote system not available") == 0);
    CHECK(strncmp(sintran_error_name(XMSG_XE_XEIMA), "XEIMA ", 6) == 0);
    CHECK(strncmp(sintran_error_name(XMSG_XE_XENSE), "XENSE ", 6) == 0);
    CHECK(strcmp(sintran_error_name(-1000), "unknown XMSG error") == 0);
}

int main(void)
{
    printf("[sintran_hdr]\n");
    captured_checksums_reproduce();
    parse_accepts_good_and_rejects_bad();
    write_fills_the_checksum();
    zero_sum_stays_zero();
    end_around_carry_is_folded();
    error_names();
    TEST_DONE();
}
