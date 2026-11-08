/*
 * test_xmsg_dg.c - datagram parse and build against the captured D100 accept and the frame shapes.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/xmsg/xmsg_dg.h"
#include "libxmsg/xmsg/xmsg_portword.h"

#include "test_util.h"

/* The TAD accept D102 sent to D100 (pcap-decode-sample.json, decoded in the plan): header with
 * F1 0x012F, sub-header 86 40 to 0064/02AB from 0066/0156, XMCSM 0400, body 00 41 00 08 ... */
#define ACCEPT_HEX                                                                                 \
    "21 13 00 0E 00 64 00 66 01 2F 04 00 D8 E5 "                                                   \
    "21 00 86 40 00 64 02 AB 00 66 01 56 04 00 "                                                   \
    "00 41 00 08 01 02 00 00 02 02 00 0A"

static void captured_accept_parses(void)
{
    uint8_t f[64];
    int n = test_hex_to_bytes(ACCEPT_HEX, f, sizeof f);
    CHECK_EQ(n, 40);
    /* The checksum in the literal above was computed by the rule; prove it is what the rule
     * gives, so the vector cannot be silently wrong. */
    CHECK_EQ(sintran_hdr_checksum(f), 0xD8E5);
    XmsgDatagram dg;
    CHECK_EQ(xmsg_dg_parse(f, (size_t)n, &dg), XMSG_OK);
    CHECK_EQ(dg.hdr.subtype, SINTRAN_SUBTYPE_DATA);
    CHECK_EQ(dg.hdr.dst_node, 100);
    CHECK_EQ(dg.hdr.src_node, 102);
    CHECK_EQ(dg.hdr.flags1, 0x012F);
    CHECK(dg.has_subhdr);
    CHECK_EQ(dg.sub.frame_flags, 0x86);
    CHECK_EQ(dg.sub.role, 0x40);
    CHECK_EQ(dg.sub.dst_system, 0x0064);
    CHECK_EQ(dg.sub.dst_port, 0x02AB);
    CHECK_EQ(dg.sub.src_system, 0x0066);
    CHECK_EQ(dg.sub.src_port, 0x0156);
    CHECK_EQ(dg.sub.xmcsm, 0x0400);
    CHECK_EQ(dg.body_len, 12);
    CHECK_BYTES(dg.body, dg.body_len, "00 41 00 08 01 02 00 00 02 02 00 0A");
    CHECK(!dg.repeat);
}

static void ack_for_the_accept(void)
{
    /* The asker's ACK: back to 102 from 100, Flags1 echoed, Flags2 0001. */
    uint8_t out[14];
    CHECK_EQ(xmsg_dg_write_short(out, SINTRAN_SUBTYPE_ACK, 102, 100, 0x012F, 0x0001), 14);
    CHECK_BYTES(out, 14, "21 13 00 03 00 66 00 64 01 2F 00 01 DC EF");
    XmsgDatagram dg;
    CHECK_EQ(xmsg_dg_parse(out, 14, &dg), XMSG_OK);
    CHECK_EQ(dg.hdr.subtype, SINTRAN_SUBTYPE_ACK);
    CHECK(!dg.has_subhdr);
    CHECK_EQ(dg.body_len, 0);
}

static void data_frame_round_trips(void)
{
    uint8_t out[700];
    uint8_t body[5] = {1, 2, 3, 4, 5};
    XmsgSubhdr sub = {0x96, 0x84, 0x0066, 0x04C2, 0x0064, 0x02AB, 5};
    size_t n = xmsg_dg_write_data(out, sizeof out, SINTRAN_SUBTYPE_DATA, 102, 100, 0x00FA, 5, &sub,
                                  body, 5);
    CHECK_EQ(n, 28 + 5);
    XmsgDatagram dg;
    CHECK_EQ(xmsg_dg_parse(out, n, &dg), XMSG_OK);
    CHECK_EQ(dg.hdr.flags2, 5);
    CHECK_EQ(dg.sub.dst_port, 0x04C2);
    CHECK_EQ(dg.sub.xmcsm, 5);
    CHECK_BYTES(dg.body, dg.body_len, "01 02 03 04 05");
    /* Too small a buffer refuses. */
    CHECK_EQ(xmsg_dg_write_data(out, 30, SINTRAN_SUBTYPE_DATA, 102, 100, 1, 5, &sub, body, 5), 0);
}

static void continuation_has_no_subheader(void)
{
    uint8_t out[700];
    uint8_t tail[3] = {9, 8, 7};
    size_t n = xmsg_dg_write_continuation(out, sizeof out, 102, 100, 0x0010, 594, tail, 3);
    CHECK_EQ(n, 14 + 3);
    XmsgDatagram dg;
    CHECK_EQ(xmsg_dg_parse(out, n, &dg), XMSG_OK);
    CHECK_EQ(dg.hdr.subtype, SINTRAN_SUBTYPE_CONTINUATION);
    CHECK_EQ(dg.hdr.flags2, 594);
    CHECK(!dg.has_subhdr);
    CHECK_BYTES(dg.body, dg.body_len, "09 08 07");
}

static void bad_frames_are_refused(void)
{
    uint8_t f[64];
    XmsgDatagram dg;
    int n = test_hex_to_bytes(ACCEPT_HEX, f, sizeof f);
    /* A data frame cut before its sub-header. */
    CHECK_EQ(xmsg_dg_parse(f, 20, &dg), XMSG_EPROTO);
    /* A wrong sub-header marker. */
    f[14] = 0x22;
    CHECK_EQ(xmsg_dg_parse(f, (size_t)n, &dg), XMSG_EPROTO);
    f[14] = 0x21;
    /* A flipped body bit does not matter to the header check (the FCS guards bodies). */
    f[30] ^= 0x01;
    CHECK_EQ(xmsg_dg_parse(f, (size_t)n, &dg), XMSG_OK);
    /* Shorter than a header. */
    CHECK_EQ(xmsg_dg_parse(f, 10, &dg), XMSG_EINVAL);
}

static void subtype_names_exist(void)
{
    CHECK(strcmp(xmsg_dg_subtype_name(SINTRAN_SUBTYPE_ACK), "ACK") == 0);
    CHECK(strcmp(xmsg_dg_subtype_name(SINTRAN_SUBTYPE_REACH_REQUEST), "REACH-REQUEST") == 0);
    CHECK(strcmp(xmsg_dg_subtype_name(0x55), "?") == 0);
}

static void portword_generator_matches_the_reference(void)
{
    /* seed * 5429 + 13849, low seven bits, 0 and 127 redrawn: replay the arithmetic here. */
    XmsgPortwordAlloc a;
    xmsg_portword_init(&a, 0x1234);
    uint16_t seed = 0x1234;
    for (int i = 0; i < 300; i++)
    {
        uint16_t index = (uint16_t)(1 + (i % 0x1FF));
        uint8_t draw;
        do
        {
            seed = (uint16_t)((uint32_t)seed * 5429u + 13849u);
            draw = (uint8_t)(seed & 0x7F);
        } while (draw == 0 || draw == 0x7F);
        uint16_t w = xmsg_portword_next(&a, index);
        CHECK_EQ(w, (index << 7) | draw);
        CHECK_EQ(xmsg_portword_index(w), index);
        CHECK_EQ(xmsg_portword_random(w), draw);
        CHECK(xmsg_portword_mintable(w));
    }
    CHECK_EQ(xmsg_portword_next(&a, 0), 0);
    CHECK_EQ(xmsg_portword_next(&a, 0x200), 0);
    CHECK(!xmsg_portword_mintable(0x0280)); /* random part 0 */
    CHECK(!xmsg_portword_mintable(0x02FF)); /* random part 127 */
    /* The low seven bits have full period: 128 draws visit every value 1..126 at least once. */
    bool seen[128] = {false};
    XmsgPortwordAlloc b;
    xmsg_portword_init(&b, 0);
    for (int i = 0; i < 128; i++)
    {
        seen[xmsg_portword_random(xmsg_portword_next(&b, 1))] = true;
    }
    for (int v = 1; v <= 126; v++)
    {
        CHECK(seen[v]);
    }
}

int main(void)
{
    printf("[xmsg_dg]\n");
    captured_accept_parses();
    ack_for_the_accept();
    data_frame_round_trips();
    continuation_has_no_subheader();
    bad_frames_are_refused();
    subtype_names_exist();
    portword_generator_matches_the_reference();
    TEST_DONE();
}
