/*
 * test_xrout_route.c - XSGSY messages and the routing-table lookup, against captured bytes.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Vectors:
 *  - the captured request/reply pair the C# pins (RetroIO
 *    RetroIO.Protocols.Xmsg/tests/ListRoutingTests.cs lines 22-33; the capture is
 *    list-routing-info-100-102-then-102-100.pcapng, frames 1 and 10 of NDInsight
 *    SINTRAN/XMSG/SRC/pcap-decode-report.txt);
 *  - the request D100 sent to node 101 on 2026-11-09 09:19:44.140 when li-rout hung (line logger
 *    frame 4905, forwarded by the XMSGGW session);
 *  - reply bodies for a neighbour, a via route and "no entry" from the decode report
 *    (li-rout-103-tree, li-route-d103-tree).
 */
#include "libxmsg/xrout/xrout_route.h"

#include "libxmsg/xmsg/xmsg_dg.h"
#include "test_util.h"

/* ListRoutingTests.cs CapturedRequestHex: 102 -> 100, asks for system 100. */
#define CAPTURED_REQUEST                                                                           \
    "21 13 00 0e 00 64 00 66 01 2b 01 00 db e9 21 00 86 84 00 64 00 00 00 66 02 a5 01 00 "        \
    "01 4b 00 04 01 02 00 64"

/* ListRoutingTests.cs CapturedResponseHex: 100 -> 102, 100 is local. */
#define CAPTURED_RESPONSE                                                                          \
    "21 13 00 0e 00 66 00 64 00 f4 01 00 dc 20 21 00 86 60 00 66 02 a5 00 66 02 a5 01 00 "        \
    "01 00 00 10 01 02 00 64 02 02 00 04 03 02 00 64 04 02 00 00"

/* Line logger frame 4905 without LAPB address/control and FCS: D100 -> 101, asks for 101. */
#define LIVE_D100_REQUEST                                                                          \
    "21 13 00 0E 00 65 00 64 00 29 01 00 DC EC 21 00 86 84 00 65 00 00 00 64 09 D4 01 00 "        \
    "01 4B 00 04 01 02 00 65"

static int hex(const char *h, uint8_t *out, size_t cap)
{
    int n = test_hex_to_bytes(h, out, cap);
    CHECK(n > 0);
    return n;
}

static void request_is_rebuilt_byte_for_byte(void)
{
    uint8_t body[XROUT_ROUTE_REQ_SIZE];
    uint8_t info[64];
    XmsgSubhdr sub = {0x86, 0x84, 0x0064, 0x0000, 0x0066, 0x02A5, 0x0100};
    xrout_route_request_write(body, 100);
    size_t n = xmsg_dg_write_data(info, sizeof info, 0x0E, 0x0064, 0x0066, 0x012B,
                                  XROUT_ROUTE_XMCSM, &sub, body, sizeof body);
    CHECK_BYTES(info, n, CAPTURED_REQUEST);
}

static void reply_is_rebuilt_byte_for_byte(void)
{
    uint8_t body[XROUT_ROUTE_REP_SIZE];
    uint8_t info[64];
    XroutRouteEntry e = {100, XROUT_REACH_LOCAL, 100, 0};
    /* Both address fields are the asker's (102 : 02A5), as in the capture. */
    XmsgSubhdr sub = {0x86, 0x60, 0x0066, 0x02A5, 0x0066, 0x02A5, 0x0100};
    xrout_route_reply_write(body, 0x01, &e);
    size_t n = xmsg_dg_write_data(info, sizeof info, 0x0E, 0x0066, 0x0064, 0x00F4,
                                  XROUT_ROUTE_XMCSM, &sub, body, sizeof body);
    CHECK_BYTES(info, n, CAPTURED_RESPONSE);
}

static void captured_frames_parse(void)
{
    uint8_t raw[64];
    XmsgDatagram dg;
    uint16_t asked = 0;
    uint8_t serial = 0;
    uint8_t status = 0xEE;
    XroutRouteEntry e;
    int n = hex(CAPTURED_REQUEST, raw, sizeof raw);

    CHECK_EQ(xmsg_dg_parse(raw, (size_t)n, &dg), XMSG_OK);
    CHECK_EQ(dg.sub.xmcsm, XROUT_ROUTE_XMCSM);
    CHECK_EQ(xrout_route_request_read(dg.body, dg.body_len, &serial, &asked), XMSG_OK);
    CHECK_EQ(serial, 1);
    CHECK_EQ(asked, 100);

    n = hex(CAPTURED_RESPONSE, raw, sizeof raw);
    CHECK_EQ(xmsg_dg_parse(raw, (size_t)n, &dg), XMSG_OK);
    CHECK_EQ(xrout_route_reply_read(dg.body, dg.body_len, &status, &e), XMSG_OK);
    CHECK_EQ(status, 0);
    CHECK_EQ(e.system, 100);
    CHECK_EQ(e.how, XROUT_REACH_LOCAL);
    CHECK_EQ(e.detail, 100);
    CHECK_EQ(e.distance, 0);

    /* The live request that hung li-rout: a valid header, asks our XROUT for system 101. */
    n = hex(LIVE_D100_REQUEST, raw, sizeof raw);
    CHECK_EQ(xmsg_dg_parse(raw, (size_t)n, &dg), XMSG_OK);
    CHECK_EQ(dg.hdr.dst_node, 101);
    CHECK_EQ(dg.hdr.src_node, 100);
    CHECK_EQ(dg.hdr.flags1, 0x0029);
    CHECK_EQ(dg.sub.dst_port, 0);
    CHECK_EQ(dg.sub.src_system, 100);
    CHECK_EQ(dg.sub.src_port, 0x09D4);
    CHECK_EQ(xrout_route_request_read(dg.body, dg.body_len, &serial, &asked), XMSG_OK);
    CHECK_EQ(asked, 101);
}

static void captured_reply_bodies(void)
{
    uint8_t body[XROUT_ROUTE_REP_SIZE];
    XroutRouteEntry e;
    uint8_t status = 0;

    /* li-rout-103-tree: 100 about its neighbour 102 on link 1. */
    XroutRouteEntry nb = {102, XROUT_REACH_NEIGHBOUR, 1, 1};
    xrout_route_reply_write(body, 1, &nb);
    CHECK_BYTES(body, sizeof body, "0100001001020066020200010302000104020001");

    /* li-route-d103-tree: 103 about 102, via 100, two hops. */
    XroutRouteEntry via = {102, XROUT_REACH_VIA, 100, 2};
    xrout_route_reply_write(body, 1, &via);
    CHECK_BYTES(body, sizeof body, "0100001001020066020200020302006404020002");

    /* li-route-d103-tree: asked 1111, nothing at or above it: status still 0, all zeros. */
    XroutRouteEntry none = {0, 0, 0, 0};
    xrout_route_reply_write(body, 1, &none);
    CHECK_BYTES(body, sizeof body, "0100001001020000020200000302000004020000");

    /* A refusal (status byte not 0) is reported, with the status. */
    body[1] = 0x02;
    CHECK_EQ(xrout_route_reply_read(body, sizeof body, &status, &e), XMSG_EPROTO);
    CHECK_EQ(status, 2);

    /* A request without parameter 1 is not readable. */
    uint8_t bad[4] = {0x01, 0x4B, 0x00, 0x00};
    uint16_t asked = 0;
    CHECK_EQ(xrout_route_request_read(bad, sizeof bad, NULL, &asked), XMSG_EPROTO);
    bad[1] = 0x41;
    CHECK_EQ(xrout_route_request_read(bad, sizeof bad, NULL, &asked), XMSG_EPROTO);
}

static void lookup_walks_in_order(void)
{
    XroutRouteTable t;
    XroutRouteEntry e;
    XroutRouteEntry add;
    xrout_route_init(&t);

    /* Added out of order; kept ascending. */
    add = (XroutRouteEntry){200, XROUT_REACH_VIA, 102, 2};
    CHECK_EQ(xrout_route_put(&t, &add), XMSG_OK);
    add = (XroutRouteEntry){101, XROUT_REACH_LOCAL, 101, 0};
    CHECK_EQ(xrout_route_put(&t, &add), XMSG_OK);
    add = (XroutRouteEntry){100, XROUT_REACH_NEIGHBOUR, 1, 1};
    CHECK_EQ(xrout_route_put(&t, &add), XMSG_OK);
    CHECK_EQ(t.count, 3);

    /* Ask 1, then answer + 1, until system 0: the whole table in order. */
    uint16_t asked = 1;
    uint16_t seen[8];
    int count = 0;
    while (xrout_route_lookup(&t, asked, &e) && count < 8)
    {
        seen[count++] = e.system;
        asked = (uint16_t)(e.system + 1u);
    }
    CHECK_EQ(count, 3);
    CHECK_EQ(seen[0], 100);
    CHECK_EQ(seen[1], 101);
    CHECK_EQ(seen[2], 200);
    /* Past the end: system 0 and all zeros. */
    CHECK(!xrout_route_lookup(&t, 201, &e));
    CHECK_EQ(e.system, 0);
    CHECK_EQ(e.how, 0);
    /* Exactly a gap: the next one above. */
    CHECK(xrout_route_lookup(&t, 102, &e));
    CHECK_EQ(e.system, 200);
    CHECK_EQ(e.detail, 102);

    /* Replace keeps one entry per system. */
    add = (XroutRouteEntry){100, XROUT_REACH_NEIGHBOUR, 2, 1};
    CHECK_EQ(xrout_route_put(&t, &add), XMSG_OK);
    CHECK_EQ(t.count, 3);
    CHECK(xrout_route_lookup(&t, 100, &e));
    CHECK_EQ(e.detail, 2);

    /* System 0 means "no entry" on the wire and cannot be added. */
    add = (XroutRouteEntry){0, XROUT_REACH_LOCAL, 0, 0};
    CHECK_EQ(xrout_route_put(&t, &add), XMSG_EINVAL);

    /* A full table says so. */
    for (uint16_t s = 300; t.count < XROUT_ROUTE_MAX; s++)
    {
        add = (XroutRouteEntry){s, XROUT_REACH_VIA, 100, 2};
        CHECK_EQ(xrout_route_put(&t, &add), XMSG_OK);
    }
    add = (XroutRouteEntry){999, XROUT_REACH_VIA, 100, 2};
    CHECK_EQ(xrout_route_put(&t, &add), XMSG_ENOSPC);
}

int main(void)
{
    request_is_rebuilt_byte_for_byte();
    reply_is_rebuilt_byte_for_byte();
    captured_frames_parse();
    captured_reply_bodies();
    lookup_walks_in_order();
    TEST_DONE();
}
