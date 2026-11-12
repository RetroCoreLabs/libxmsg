/*
 * test_xrout_name.c - XSGNI messages and the name-table lookup, against captured bytes.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Vectors: the complete LIST-NAMES walk captured 2026-11-10 on the COSMOS Ethernet hub (TCP
 * 5010, listnames-hub.pcapng): D102 read D100's name table, 14 requests and 14 replies. Each
 * string is the SINTRAN datagram as it crossed the hub (14-byte header, 14-byte sub-header,
 * body), frame number from the capture in the comment. The header checksum is part of each, so
 * a rebuilt datagram that matches also has the right checksum.
 */
#include "libxmsg/xrout/xrout_name.h"

#include "libxmsg/xmsg/xmsg_dg.h"
#include "test_util.h"

/* #27: the first request, 102 -> 100, F1 0x0012, parameter 1 = 16-bit 0. */
#define REQ_FIRST                                                                                  \
    "21 13 00 0E 00 64 00 66 00 12 01 00 DD 02 21 00 86 84 00 64 00 00 00 66 06 B2 01 00 "         \
    "01 45 00 04 01 02 00 00"
/* #75: the second request, magic 0064 0001 + 1. */
#define REQ_SECOND                                                                                 \
    "21 13 00 0E 00 64 00 66 00 13 01 00 DD 01 21 00 86 84 00 64 00 00 00 66 06 B2 01 00 "         \
    "01 45 00 06 01 04 00 64 00 02"
/* #651: the last request, after D19999. */
#define REQ_LAST                                                                                   \
    "21 13 00 0E 00 64 00 66 00 1F 01 00 DC F5 21 00 86 84 00 64 00 00 00 66 06 B2 01 00 "         \
    "01 45 00 06 01 04 4E 1F 00 02"

/* Replies, 100 -> 102, on D100's own count. */
#define REP_D100                                                                                   \
    "21 13 00 0E 00 66 00 64 00 11 01 00 DD 03 21 00 86 60 00 66 06 B2 00 66 06 B2 01 00 "         \
    "01 00 00 0C 01 04 00 64 00 01 FE 04 44 31 30 30"
#define REP_XFTRA                                                                                  \
    "21 13 00 0E 00 66 00 64 00 13 01 00 DD 01 21 00 86 60 00 66 06 B2 00 66 06 B2 01 00 "         \
    "01 00 00 12 01 04 00 64 02 27 FE 06 2A 58 46 54 52 41 03 02 00 01"
#define REP_XM_ENNS0                                                                               \
    "21 13 00 0E 00 66 00 64 00 14 01 00 DD 00 21 00 86 60 00 66 06 B2 00 66 06 B2 01 00 "         \
    "01 00 00 12 01 04 00 64 02 D0 FE 09 2A 58 4D 2D 45 4E 4E 53 30 00"
#define REP_TADADM                                                                                 \
    "21 13 00 0E 00 66 00 64 00 16 01 00 DC FE 21 00 86 60 00 66 06 B2 00 66 06 B2 01 00 "         \
    "01 00 00 10 01 04 00 64 04 01 FE 07 2A 54 41 44 41 44 4D 00"
#define REP_FA_FSA                                                                                 \
    "21 13 00 0E 00 66 00 64 00 17 01 00 DC FD 21 00 86 60 00 66 06 B2 00 66 06 B2 01 00 "         \
    "01 00 00 14 01 04 00 64 04 CE FE 07 2A 46 41 2D 46 53 41 00 03 02 00 02"
#define REP_FA_FSA_I                                                                               \
    "21 13 00 0E 00 66 00 64 00 18 01 00 DC FC 21 00 86 60 00 66 06 B2 00 66 06 B2 01 00 "         \
    "01 00 00 16 01 04 00 64 05 3F FE 09 2A 46 41 2D 46 53 41 2D 49 00 03 02 00 00"
#define REP_D19999                                                                                 \
    "21 13 00 0E 00 66 00 64 00 1D 01 00 DC F7 21 00 86 60 00 66 06 B2 00 66 06 B2 01 00 "         \
    "01 00 00 0E 01 04 4E 1F 00 01 FE 06 44 31 39 39 39 39"
#define REP_END                                                                                    \
    "21 13 00 0E 00 66 00 64 00 1E 01 00 DC F6 21 00 86 60 00 66 06 B2 00 66 06 B2 01 00 "         \
    "01 00 00 04 01 02 00 00"

static size_t build_request(uint8_t *out, size_t cap, uint16_t f1, uint32_t asked)
{
    uint8_t body[XROUT_NAME_REQ_MAX];
    XmsgSubhdr sub = {0x86, 0x84, 0x0064, 0x0000, 0x0066, 0x06B2, 0x0100};
    size_t len = xrout_name_request_write(body, asked);
    return xmsg_dg_write_data(out, cap, 0x0E, 0x0064, 0x0066, f1, XROUT_NAME_XMCSM, &sub, body,
                              len);
}

static size_t build_reply(uint8_t *out, size_t cap, uint16_t f1, const XroutNameRow *r)
{
    uint8_t body[XROUT_NAME_REP_MAX];
    XmsgSubhdr sub = {0x86, 0x60, 0x0066, 0x06B2, 0x0066, 0x06B2, 0x0100};
    size_t len = xrout_name_reply_write(body, 0x01, r);
    CHECK(len > 0);
    return xmsg_dg_write_data(out, cap, 0x0E, 0x0066, 0x0064, f1, XROUT_NAME_XMCSM, &sub, body,
                              len);
}

static void requests_are_rebuilt_byte_for_byte(void)
{
    uint8_t info[64];
    size_t n = build_request(info, sizeof info, 0x0012, 0);
    CHECK_BYTES(info, n, REQ_FIRST);
    n = build_request(info, sizeof info, 0x0013, 0x00640002u);
    CHECK_BYTES(info, n, REQ_SECOND);
    n = build_request(info, sizeof info, 0x001F, 0x4E1F0002u);
    CHECK_BYTES(info, n, REQ_LAST);
}

static void replies_are_rebuilt_byte_for_byte(void)
{
    uint8_t info[80];
    XroutNameRow r;
    size_t n;

    CHECK_EQ(xrout_name_row_set(&r, 100, 0x0001, "D100", XROUT_NAME_NO_FREE), XMSG_OK);
    n = build_reply(info, sizeof info, 0x0011, &r);
    CHECK_BYTES(info, n, REP_D100);
    /* Even name, free seats present. */
    CHECK_EQ(xrout_name_row_set(&r, 100, 0x0227, "*XFTRA", 1), XMSG_OK);
    n = build_reply(info, sizeof info, 0x0013, &r);
    CHECK_BYTES(info, n, REP_XFTRA);
    /* Odd name and nothing after it: the pad byte is still there and counted. */
    CHECK_EQ(xrout_name_row_set(&r, 100, 0x02D0, "*XM-ENNS0", XROUT_NAME_NO_FREE), XMSG_OK);
    n = build_reply(info, sizeof info, 0x0014, &r);
    CHECK_BYTES(info, n, REP_XM_ENNS0);
    CHECK_EQ(xrout_name_row_set(&r, 100, 0x0401, "*TADADM", XROUT_NAME_NO_FREE), XMSG_OK);
    n = build_reply(info, sizeof info, 0x0016, &r);
    CHECK_BYTES(info, n, REP_TADADM);
    /* Odd name, pad, then the free element. */
    CHECK_EQ(xrout_name_row_set(&r, 100, 0x04CE, "*FA-FSA", 2), XMSG_OK);
    n = build_reply(info, sizeof info, 0x0017, &r);
    CHECK_BYTES(info, n, REP_FA_FSA);
    /* Free seats 0 is a count, not "none". */
    CHECK_EQ(xrout_name_row_set(&r, 100, 0x053F, "*FA-FSA-I", 0), XMSG_OK);
    n = build_reply(info, sizeof info, 0x0018, &r);
    CHECK_BYTES(info, n, REP_FA_FSA_I);
    CHECK_EQ(xrout_name_row_set(&r, 19999, 0x0001, "D19999", XROUT_NAME_NO_FREE), XMSG_OK);
    n = build_reply(info, sizeof info, 0x001D, &r);
    CHECK_BYTES(info, n, REP_D19999);

    /* The end form. */
    uint8_t body[XROUT_NAME_REP_MAX];
    XmsgSubhdr sub = {0x86, 0x60, 0x0066, 0x06B2, 0x0066, 0x06B2, 0x0100};
    size_t len = xrout_name_reply_write(body, 0x01, NULL);
    CHECK_EQ(len, 8);
    n = xmsg_dg_write_data(info, sizeof info, 0x0E, 0x0066, 0x0064, 0x001E, XROUT_NAME_XMCSM, &sub,
                           body, len);
    CHECK_BYTES(info, n, REP_END);
}

/* Parse a captured datagram and read its reply body. */
static int read_captured_reply(const char *hexs, XroutNameRow *r, bool *is_end)
{
    uint8_t raw[80];
    XmsgDatagram dg;
    uint8_t status = 0xEE;
    int n = test_hex_to_bytes(hexs, raw, sizeof raw);
    CHECK(n > 0);
    CHECK_EQ(xmsg_dg_parse(raw, (size_t)n, &dg), XMSG_OK);
    CHECK_EQ(dg.sub.xmcsm, XROUT_NAME_XMCSM);
    int rc = xrout_name_reply_read(dg.body, dg.body_len, &status, r, is_end);
    CHECK_EQ(status, 0);
    return rc;
}

static void captured_frames_parse(void)
{
    uint8_t raw[64];
    XmsgDatagram dg;
    XroutNameRow r;
    bool is_end = true;
    uint32_t asked = 99;
    uint8_t serial = 0;
    int n = test_hex_to_bytes(REQ_FIRST, raw, sizeof raw);

    CHECK_EQ(xmsg_dg_parse(raw, (size_t)n, &dg), XMSG_OK);
    CHECK_EQ(dg.sub.dst_port, 0);
    CHECK_EQ(xrout_name_request_read(dg.body, dg.body_len, &serial, &asked), XMSG_OK);
    CHECK_EQ(serial, 1);
    CHECK_EQ(asked, 0);
    n = test_hex_to_bytes(REQ_LAST, raw, sizeof raw);
    CHECK_EQ(xmsg_dg_parse(raw, (size_t)n, &dg), XMSG_OK);
    CHECK_EQ(xrout_name_request_read(dg.body, dg.body_len, &serial, &asked), XMSG_OK);
    CHECK_EQ(asked, 0x4E1F0002u);

    CHECK_EQ(read_captured_reply(REP_D100, &r, &is_end), XMSG_OK);
    CHECK(!is_end);
    CHECK_EQ(r.system, 100);
    CHECK_EQ(r.port, 0x0001);
    CHECK(xrout_name_is_system(&r));
    CHECK(strcmp(r.name, "D100") == 0);
    CHECK_EQ(r.free, XROUT_NAME_NO_FREE);

    CHECK_EQ(read_captured_reply(REP_TADADM, &r, &is_end), XMSG_OK);
    CHECK(strcmp(r.name, "*TADADM") == 0);
    CHECK_EQ(r.port, 0x0401);
    CHECK_EQ(r.port >> 7u, 8); /* what LIST-NAMES prints as the port */
    CHECK_EQ(r.free, XROUT_NAME_NO_FREE);

    CHECK_EQ(read_captured_reply(REP_FA_FSA, &r, &is_end), XMSG_OK);
    CHECK(strcmp(r.name, "*FA-FSA") == 0);
    CHECK_EQ(r.free, 2);
    CHECK_EQ(read_captured_reply(REP_FA_FSA_I, &r, &is_end), XMSG_OK);
    CHECK(strcmp(r.name, "*FA-FSA-I") == 0);
    CHECK_EQ(r.free, 0);
    CHECK_EQ(read_captured_reply(REP_XM_ENNS0, &r, &is_end), XMSG_OK);
    CHECK(strcmp(r.name, "*XM-ENNS0") == 0);
    CHECK_EQ(r.free, XROUT_NAME_NO_FREE);
    CHECK_EQ(read_captured_reply(REP_D19999, &r, &is_end), XMSG_OK);
    CHECK_EQ(r.system, 19999);
    CHECK_EQ(xrout_name_magic(&r), 0x4E1F0001u);

    CHECK_EQ(read_captured_reply(REP_END, &r, &is_end), XMSG_OK);
    CHECK(is_end);
    CHECK_EQ(r.system, 0);
}

static void bad_bodies_are_refused(void)
{
    XroutNameRow r;
    bool is_end = false;
    uint8_t status = 0;
    uint32_t asked = 0;

    /* A refusal (status not 0) is reported, with the status. */
    uint8_t refused[8] = {0x01, 0x02, 0x00, 0x04, 0x01, 0x02, 0x00, 0x00};
    CHECK_EQ(xrout_name_reply_read(refused, sizeof refused, &status, &r, &is_end), XMSG_EPROTO);
    CHECK_EQ(status, 2);

    /* A row with no name. */
    uint8_t no_name[10] = {0x01, 0x00, 0x00, 0x06, 0x01, 0x04, 0x00, 0x64, 0x00, 0x01};
    CHECK_EQ(xrout_name_reply_read(no_name, sizeof no_name, &status, &r, &is_end), XMSG_EPROTO);

    /* A name longer than a row holds is refused, not cut. */
    uint8_t long_name[4 + 6 + 2 + 18];
    memset(long_name, 'A', sizeof long_name);
    long_name[0] = 0x01;
    long_name[1] = 0x00;
    long_name[2] = 0x00;
    long_name[3] = (uint8_t)(sizeof long_name - 4u);
    long_name[4] = 0x01;
    long_name[5] = 0x04;
    long_name[6] = 0x00;
    long_name[7] = 0x64;
    long_name[8] = 0x00;
    long_name[9] = 0x01;
    long_name[10] = 0xFE;
    long_name[11] = 18;
    CHECK_EQ(xrout_name_reply_read(long_name, sizeof long_name, &status, &r, &is_end), XMSG_EPROTO);

    /* Requests: no parameter 1, or not XSGNI. */
    uint8_t bad[4] = {0x01, 0x45, 0x00, 0x00};
    CHECK_EQ(xrout_name_request_read(bad, sizeof bad, NULL, &asked), XMSG_EPROTO);
    uint8_t route[8] = {0x01, 0x4B, 0x00, 0x04, 0x01, 0x02, 0x00, 0x00};
    CHECK_EQ(xrout_name_request_read(route, sizeof route, NULL, &asked), XMSG_EPROTO);
    CHECK_EQ(xrout_name_request_read(route, 3, NULL, &asked), XMSG_EINVAL);

    /* A system at or above 0x8000 keeps all 32 bits of its magic. */
    uint8_t body[XROUT_NAME_REP_MAX];
    CHECK_EQ(xrout_name_row_set(&r, 0x9001, 0x0001, "D36865", XROUT_NAME_NO_FREE), XMSG_OK);
    size_t len = xrout_name_reply_write(body, 0x01, &r);
    CHECK_EQ(xrout_name_reply_read(body, len, &status, &r, &is_end), XMSG_OK);
    CHECK_EQ(xrout_name_magic(&r), 0x90010001u);
    len = xrout_name_request_write(body, 0x90010002u);
    CHECK_EQ(xrout_name_request_read(body, len, NULL, &asked), XMSG_OK);
    CHECK_EQ(asked, 0x90010002u);
}

static void table_keeps_magic_order(void)
{
    XroutNameTable t;
    XroutNameRow r;
    xrout_name_init(&t);

    /* Added out of order; kept ascending by system, then port word. */
    CHECK_EQ(xrout_name_row_set(&r, 101, 0x0001, "D101", XROUT_NAME_NO_FREE), XMSG_OK);
    CHECK_EQ(xrout_name_put(&t, &r), XMSG_OK);
    CHECK_EQ(xrout_name_row_set(&r, 100, 0x0401, "*TADADM", XROUT_NAME_NO_FREE), XMSG_OK);
    CHECK_EQ(xrout_name_put(&t, &r), XMSG_OK);
    CHECK_EQ(xrout_name_row_set(&r, 100, 0x0001, "D100", XROUT_NAME_NO_FREE), XMSG_OK);
    CHECK_EQ(xrout_name_put(&t, &r), XMSG_OK);
    CHECK_EQ(t.count, 3);

    /* Ask 0, then magic + 1, until none: the whole table in order. */
    const char *expect[3] = {"D100", "*TADADM", "D101"};
    uint32_t asked = 0;
    int count = 0;
    while (xrout_name_lookup(&t, asked, &r) && count < 3)
    {
        CHECK(strcmp(r.name, expect[count]) == 0);
        count++;
        asked = xrout_name_magic(&r) + 1u;
    }
    CHECK_EQ(count, 3);
    CHECK(!xrout_name_lookup(&t, asked, &r));
    CHECK_EQ(r.system, 0);
    CHECK_EQ(r.free, XROUT_NAME_NO_FREE);
    /* Between two rows: the next one above. */
    CHECK(xrout_name_lookup(&t, 0x00640002u, &r));
    CHECK(strcmp(r.name, "*TADADM") == 0);

    /* Replace keeps one row per magic. */
    CHECK_EQ(xrout_name_row_set(&r, 100, 0x0401, "*TADADM", 5), XMSG_OK);
    CHECK_EQ(xrout_name_put(&t, &r), XMSG_OK);
    CHECK_EQ(t.count, 3);
    CHECK(xrout_name_has(&t, 100, 0x0401));
    CHECK(xrout_name_lookup(&t, 0x00640401u, &r));
    CHECK_EQ(r.free, 5);

    /* Remove. */
    CHECK_EQ(xrout_name_remove(&t, 100, 0x0401), XMSG_OK);
    CHECK_EQ(xrout_name_remove(&t, 100, 0x0401), XMSG_EINVAL);
    CHECK(!xrout_name_has(&t, 100, 0x0401));
    CHECK_EQ(t.count, 2);

    /* Rows that cannot go on the wire. */
    CHECK_EQ(xrout_name_row_set(&r, 0, 0x0001, "D0", XROUT_NAME_NO_FREE), XMSG_EINVAL);
    CHECK_EQ(xrout_name_row_set(&r, 100, 0, "X", XROUT_NAME_NO_FREE), XMSG_EINVAL);
    CHECK_EQ(xrout_name_row_set(&r, 100, 1, "", XROUT_NAME_NO_FREE), XMSG_EINVAL);
    CHECK_EQ(xrout_name_row_set(&r, 100, 1, "ABCDEFGHIJKLMNOPQ", XROUT_NAME_NO_FREE), XMSG_EINVAL);
    CHECK_EQ(xrout_name_row_set(&r, 100, 1, "ABCDEFGHIJKLMNOP", XROUT_NAME_NO_FREE), XMSG_OK);
    CHECK_EQ(xrout_name_row_set(&r, 100, 1, "X", 0x10000), XMSG_EINVAL);
    CHECK_EQ(xrout_name_row_set(&r, 100, 1, "X", -2), XMSG_EINVAL);
    CHECK_EQ(xrout_name_reply_write(NULL, 1, NULL), 0);

    /* A full table says so. */
    for (uint16_t s = 300; t.count < XROUT_NAME_ROWS_MAX; s++)
    {
        CHECK_EQ(xrout_name_row_set(&r, s, 0x0001, "DX", XROUT_NAME_NO_FREE), XMSG_OK);
        CHECK_EQ(xrout_name_put(&t, &r), XMSG_OK);
    }
    CHECK_EQ(xrout_name_row_set(&r, 999, 0x0001, "D999", XROUT_NAME_NO_FREE), XMSG_OK);
    CHECK_EQ(xrout_name_put(&t, &r), XMSG_ENOSPC);
}

int main(void)
{
    requests_are_rebuilt_byte_for_byte();
    replies_are_rebuilt_byte_for_byte();
    captured_frames_parse();
    bad_bodies_are_refused();
    table_keeps_magic_order();
    TEST_DONE();
}
