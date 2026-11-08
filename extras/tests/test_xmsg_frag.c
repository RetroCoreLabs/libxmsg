/*
 * test_xmsg_frag.c - a 1032-byte message split into 594 + 438 and joined again; the drop rules.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/xmsg/xmsg_frag.h"

#include "test_util.h"

static uint8_t s_first[700];
static uint8_t s_cont[700];

static size_t build_pair(uint16_t flags1, const uint8_t *body, size_t len, size_t *cont_len)
{
    XmsgSubhdr sub = {0x96, 0x84, 100, 0x0211, 102, 0x05B9, (uint16_t)len};
    size_t n1 = xmsg_dg_write_data(s_first, sizeof s_first, SINTRAN_SUBTYPE_FIRST_FRAG, 100, 102,
                                   flags1, (uint16_t)len, &sub, body, XMSG_DG_FIRST_FRAG_BODY);
    *cont_len = xmsg_dg_write_continuation(
        s_cont, sizeof s_cont, 100, 102, flags1, (uint16_t)XMSG_DG_FIRST_FRAG_BODY,
        body + XMSG_DG_FIRST_FRAG_BODY, len - XMSG_DG_FIRST_FRAG_BODY);
    return n1;
}

static void a_file_data_message_is_joined(void)
{
    uint8_t body[1032];
    for (size_t i = 0; i < sizeof body; i++)
    {
        body[i] = (uint8_t)(i * 7);
    }
    size_t n2;
    size_t n1 = build_pair(0x0042, body, sizeof body, &n2);
    CHECK_EQ(n1, 28 + 594);
    CHECK_EQ(n2, 14 + 438);

    XmsgReassembler r;
    xmsg_reasm_init(&r);
    XmsgDatagram in;
    XmsgDatagram out;
    CHECK_EQ(xmsg_dg_parse(s_first, n1, &in), XMSG_OK);
    CHECK_EQ(in.hdr.flags2, 1032); /* first fragment declares the total */
    CHECK_EQ(xmsg_reasm_accept(&r, &in, &out, 1000), 0);
    CHECK_EQ(xmsg_dg_parse(s_cont, n2, &in), XMSG_OK);
    CHECK_EQ(in.hdr.flags2, 594); /* continuation declares where it resumes */
    CHECK_EQ(xmsg_reasm_accept(&r, &in, &out, 1001), 1);
    CHECK_EQ(out.hdr.subtype, SINTRAN_SUBTYPE_DATA);
    CHECK_EQ(out.hdr.flags1, 0x0042);
    CHECK(out.has_subhdr);
    CHECK_EQ(out.sub.dst_port, 0x0211);
    CHECK_EQ(out.sub.xmcsm, 1032);
    CHECK_EQ(out.body_len, 1032);
    CHECK(memcmp(out.body, body, 1032) == 0);
    CHECK_EQ(r.joined, 1);
}

static void whole_frames_pass_through(void)
{
    uint8_t f[14];
    xmsg_dg_write_short(f, SINTRAN_SUBTYPE_ACK, 100, 102, 5, 1);
    XmsgReassembler r;
    xmsg_reasm_init(&r);
    XmsgDatagram in;
    XmsgDatagram out;
    CHECK_EQ(xmsg_dg_parse(f, 14, &in), XMSG_OK);
    CHECK_EQ(xmsg_reasm_accept(&r, &in, &out, 0), 1);
    CHECK_EQ(out.hdr.subtype, SINTRAN_SUBTYPE_ACK);
}

static void orphan_and_mismatch_are_dropped(void)
{
    uint8_t body[1032] = {0};
    size_t n2;
    size_t n1 = build_pair(0x0007, body, sizeof body, &n2);
    XmsgReassembler r;
    xmsg_reasm_init(&r);
    XmsgDatagram in;
    XmsgDatagram out;
    /* A continuation with nothing held. */
    CHECK_EQ(xmsg_dg_parse(s_cont, n2, &in), XMSG_OK);
    CHECK_EQ(xmsg_reasm_accept(&r, &in, &out, 0), XMSG_EPROTO);
    CHECK_EQ(r.orphaned, 1);
    /* A continuation whose offset disagrees with the held part. */
    CHECK_EQ(xmsg_dg_parse(s_first, n1, &in), XMSG_OK);
    CHECK_EQ(xmsg_reasm_accept(&r, &in, &out, 0), 0);
    uint8_t wrong[700];
    size_t nw = xmsg_dg_write_continuation(wrong, sizeof wrong, 100, 102, 0x0007, 600, body, 10);
    CHECK_EQ(xmsg_dg_parse(wrong, nw, &in), XMSG_OK);
    CHECK_EQ(xmsg_reasm_accept(&r, &in, &out, 0), XMSG_EPROTO);
    CHECK_EQ(r.mismatched, 1);
    /* The held part was discarded with it: the right continuation is now an orphan. */
    CHECK_EQ(xmsg_dg_parse(s_cont, n2, &in), XMSG_OK);
    CHECK_EQ(xmsg_reasm_accept(&r, &in, &out, 0), XMSG_EPROTO);
    CHECK_EQ(r.orphaned, 2);
}

static void a_repeated_first_fragment_replaces_the_held_one(void)
{
    uint8_t body[1032];
    memset(body, 0xAB, sizeof body);
    size_t n2;
    size_t n1 = build_pair(0x0009, body, sizeof body, &n2);
    XmsgReassembler r;
    xmsg_reasm_init(&r);
    XmsgDatagram in;
    XmsgDatagram out;
    CHECK_EQ(xmsg_dg_parse(s_first, n1, &in), XMSG_OK);
    CHECK_EQ(xmsg_reasm_accept(&r, &in, &out, 0), 0);
    CHECK_EQ(xmsg_reasm_accept(&r, &in, &out, 1), 0); /* the same first fragment again */
    CHECK_EQ(r.evicted, 0);
    CHECK_EQ(xmsg_dg_parse(s_cont, n2, &in), XMSG_OK);
    CHECK_EQ(xmsg_reasm_accept(&r, &in, &out, 2), 1);
    CHECK_EQ(out.body_len, 1032);
}

static void a_new_message_evicts_the_oldest_when_full(void)
{
    uint8_t body[1032] = {0};
    size_t n2;
    XmsgReassembler r;
    xmsg_reasm_init(&r);
    XmsgDatagram in;
    XmsgDatagram out;
    for (uint16_t k = 0; k < XMSG_REASSEMBLY_SLOTS + 1; k++)
    {
        size_t n1 = build_pair((uint16_t)(0x100 + k), body, sizeof body, &n2);
        CHECK_EQ(xmsg_dg_parse(s_first, n1, &in), XMSG_OK);
        CHECK_EQ(xmsg_reasm_accept(&r, &in, &out, k), 0);
    }
    CHECK_EQ(r.evicted, 1);
    /* The first one is gone; the last one is still joinable. */
    build_pair(0x0100, body, sizeof body, &n2);
    CHECK_EQ(xmsg_dg_parse(s_cont, n2, &in), XMSG_OK);
    CHECK_EQ(xmsg_reasm_accept(&r, &in, &out, 9), XMSG_EPROTO);
    build_pair((uint16_t)(0x100 + XMSG_REASSEMBLY_SLOTS), body, sizeof body, &n2);
    CHECK_EQ(xmsg_dg_parse(s_cont, n2, &in), XMSG_OK);
    CHECK_EQ(xmsg_reasm_accept(&r, &in, &out, 9), 1);
}

int main(void)
{
    printf("[xmsg_frag]\n");
    a_file_data_message_is_joined();
    whole_frames_pass_through();
    orphan_and_mismatch_are_dropped();
    a_repeated_first_fragment_replaces_the_held_one();
    a_new_message_evicts_the_oldest_when_full();
    TEST_DONE();
}
