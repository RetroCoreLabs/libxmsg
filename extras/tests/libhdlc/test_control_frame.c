/*
 * test_control_frame.c - command (control) frames between a gateway and its host PC.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Every expected byte string below was computed with a separate bit-serial CRC in Python
 * (python -I, 2026-11-09), not by this library. The same strings are the worked examples in
 * extras/docs/control-frames.md, so the document cannot drift from the code without this test
 * failing.
 */
#include "libhdlc/byte_frame.h"
#include "libhdlc/control_frame.h"
#include "libhdlc/hdlc.h"
#include "test_util.h"

static void build_examples(void)
{
    uint8_t f[64];
    uint8_t st[2 + 2 * 64];
    static const uint8_t ok[] = { 'O', 'K' };
    static const uint8_t level[] = { 3 };
    static const uint8_t period[] = { 0xE8, 0x03 };
    int n;

    n = control_frame_build(CONTROL_FRAME_PING, 7, 0, NULL, 0, f);
    CHECK_BYTES(f, (size_t)n, "FE 05 07 00 FD AE");
    n = byte_frame_encode(f, n, st);
    CHECK_BYTES(st, (size_t)n, "7E FE 7D 25 7D 27 7D 20 FD AE 7E");

    n = control_frame_build(CONTROL_FRAME_HELLO, 1, 0, NULL, 0, f);
    CHECK_BYTES(f, (size_t)n, "FE 01 01 00 4C 99");

    n = control_frame_build(CONTROL_FRAME_REPLY, 7, CONTROL_FRAME_FLAG_LAST, ok, 2, f);
    CHECK_BYTES(f, (size_t)n, "FE 82 07 01 4F 4B 28 59");

    n = control_frame_build(CONTROL_FRAME_LOG_LEVEL, 2, 0, level, 1, f);
    CHECK_BYTES(f, (size_t)n, "FE 03 02 00 03 78 6C");

    n = control_frame_build(CONTROL_FRAME_STAT_PUSH, 3, 0, period, 2, f);
    CHECK_BYTES(f, (size_t)n, "FE 04 03 00 E8 03 6E EC");
    n = byte_frame_encode(f, n, st);
    CHECK_BYTES(st, (size_t)n, "7E FE 7D 24 7D 23 7D 20 E8 7D 23 6E EC 7E");
}

static void parse_and_the_two_locks(void)
{
    static const uint8_t reply[] = { 0xFE, 0x82, 0x07, 0x01, 0x4F, 0x4B, 0x28, 0x59 };
    uint8_t data[8];
    control_frame cf;
    uint16_t crc;

    CHECK(control_frame_parse(reply, (int)sizeof reply, &cf));
    CHECK_EQ(cf.type, CONTROL_FRAME_REPLY);
    CHECK_EQ(cf.id, 7);
    CHECK_EQ(cf.flags, CONTROL_FRAME_FLAG_LAST);
    CHECK_EQ(cf.payload_len, 2);
    CHECK(memcmp(cf.payload, "OK", 2) == 0);
    CHECK_EQ(control_frame_crc(reply, (int)sizeof reply), CONTROL_FRAME_RESIDUE);
    CHECK(!control_frame_is_data(reply, (int)sizeof reply));

    /* Lock 1: any other address is data, even with a control-frame check. */
    memcpy(data, reply, sizeof reply);
    data[0] = 0x01;
    CHECK(!control_frame_parse(data, (int)sizeof data, &cf));

    /* Lock 2: the same bytes at address 0xFE with a NORMAL (inverted) HDLC FCS is data. */
    crc = HDLC_CRC_INIT;
    for (int i = 0; i < 6; i++)
    {
        crc = hdlc_crc_byte(crc, reply[i]);
    }
    crc = (uint16_t)~crc;
    memcpy(data, reply, 6);
    data[6] = (uint8_t)(crc & 0xFFu);
    data[7] = (uint8_t)(crc >> 8u);
    CHECK(control_frame_is_data(data, 8));
    CHECK(!control_frame_parse(data, 8, &cf));

    /* Too short, and a damaged check. */
    CHECK(!control_frame_parse(reply, CONTROL_FRAME_OVERHEAD - 1, &cf));
    memcpy(data, reply, sizeof reply);
    data[4] ^= 0x01u;
    CHECK(!control_frame_parse(data, (int)sizeof data, &cf));
}

static void payload_limits(void)
{
    static uint8_t big[CONTROL_FRAME_MAX_PAYLOAD + 1];
    static uint8_t f[BYTE_FRAME_MAX + 8];
    control_frame cf;
    int n = control_frame_build(CONTROL_FRAME_COMMAND, 9, 0, big, CONTROL_FRAME_MAX_PAYLOAD, f);
    CHECK_EQ(n, BYTE_FRAME_MAX);
    CHECK(control_frame_parse(f, n, &cf));
    CHECK_EQ(cf.payload_len, CONTROL_FRAME_MAX_PAYLOAD);
    CHECK_EQ(control_frame_build(CONTROL_FRAME_COMMAND, 9, 0, big, CONTROL_FRAME_MAX_PAYLOAD + 1, f),
             -1);
    CHECK_EQ(control_frame_build(CONTROL_FRAME_COMMAND, 9, 0, big, -1, f), -1);
}

int main(void)
{
    build_examples();
    parse_and_the_two_locks();
    payload_limits();
    TEST_DONE();
}
