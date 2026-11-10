/*
 * test_smoke.c - the public headers are self-contained, guarded, and the generated accessors read
 * a real captured frame correctly.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Including every public header twice proves the include guards; including xmsg.h first proves
 * each header brings in what it needs (house rule 4.2). The frame below is the D100 accept the
 * registry README cites as the generator's own check: destination 102, source 100, Flags1 0x0021,
 * checksum 0xD9F3. Flags2 (0x0400) follows from the checksum rule and is verified in P3.
 */
#include "libxmsg/xmsg.h"
#include "libxmsg/xmsg.h"
#include "libxmsg/xmsg_config.h"
#include "libxmsg/xmsg_endian.h"
#include "libxmsg/xmsg_ports.h"
#include "libxmsg/gen/sintran_wire.h"
#include "libxmsg/gen/tad_wire.h"
#include "libxmsg/gen/xrout_services.h"
#include "libxmsg/gen/fa_qform.h"
#include "libxmsg/gen/chat_wire.h"

#include "test_util.h"

static const uint8_t D100_ACCEPT_HEADER[SINTRAN_HEADER_SIZE] = {
    0x21, 0x13, 0x00, 0x0E, 0x00, 0x66, 0x00, 0x64, 0x00, 0x21, 0x04, 0x00, 0xD9, 0xF3};

static void generated_accessors_read_the_captured_accept(void)
{
    CHECK_EQ(SINTRAN_HEADER_SIZE, 14);
    CHECK_EQ(sintran_header_read_marker(D100_ACCEPT_HEADER), 0x2113);
    CHECK_EQ(sintran_header_read_packet_type_and_subtype(D100_ACCEPT_HEADER), 0x000E);
    CHECK_EQ(sintran_header_read_destination_node(D100_ACCEPT_HEADER), 102);
    CHECK_EQ(sintran_header_read_source_node(D100_ACCEPT_HEADER), 100);
    CHECK_EQ(sintran_header_read_flags1(D100_ACCEPT_HEADER), 0x0021);
    CHECK_EQ(sintran_header_read_checksum(D100_ACCEPT_HEADER), 0xD9F3);
}

static void generated_constants_match_the_registry(void)
{
    /* Values quoted straight from the registry markdown (DOC/protocols) so a regenerated header
     * that drifted from the registry fails here before anything is built on it. */
    CHECK_EQ(XMSG_FRAME_FLAG_SYSTEM_MODE, 0x80);
    CHECK_EQ(XMSG_FRAME_FLAG_DATA_PHASE, 0x10);
    CHECK_EQ(ND_LINK_HEADER_SIZE, 11);
}

static void endian_helpers_round_trip(void)
{
    uint8_t buf[4];
    xmsg_be16_write(buf, 0x4E1F);
    CHECK_BYTES(buf, 2, "4E 1F");
    CHECK_EQ(xmsg_be16_read(buf), 0x4E1F);
    xmsg_be32_write(buf, 0x0100014Bu);
    CHECK_BYTES(buf, 4, "01 00 01 4B");
    CHECK_EQ(xmsg_be32_read(buf), 0x0100014Bu);
}

static void config_defaults_are_visible(void)
{
    CHECK(XMSG_MAX_SESSIONS >= 1);
    CHECK(XMSG_LAPB_N1 >= 4);
    CHECK_EQ(XMSG_LOG_CATEGORY_COUNT, 11);
    CHECK(strcmp(XMSG_VERSION_STRING, "0.1.0") == 0);
}

int main(void)
{
    printf("[smoke]\n");
    generated_accessors_read_the_captured_accept();
    generated_constants_match_the_registry();
    endian_helpers_round_trip();
    config_defaults_are_visible();
    TEST_DONE();
}
