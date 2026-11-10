/*
 * test_pair_route.c - routing questions end to end: our node answers a host's XSGSY, and our walk
 *                     reads a table, both over the in-memory pair.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Node A is 101 (us). Node B is 100 and plays the host: for the answer tests it puts the EXACT
 * bytes D100 sent on 2026-11-09 (line logger frame 4905, the request behind li-rout's missing A:
 * line) into its LAPB window, so the real request crosses the wire to A, and it catches A's reply
 * as an unclaimed datagram. A's count to 100 is set to 0x0028, its real value at that moment
 * (frame 4899: our last data frame to D100 was 0x0027), so the reply's number is checked against
 * the rule from the captures: our own count, not the request's 0x0029.
 */
#include "libxmsg/xmsg/xmsg_node.h"
#include "libxmsg/xrout/xrout_walk.h"

#include "pair/test_pair.h"
#include "test_util.h"

/* Frame 4905 minus LAPB address/control and FCS. */
#define LIVE_D100_REQUEST                                                                          \
    "21 13 00 0E 00 65 00 64 00 29 01 00 DC EC 21 00 86 84 00 65 00 00 00 64 09 D4 01 00 "        \
    "01 4B 00 04 01 02 00 65"

/* The answer: our F1 0x0028, sub-header 86 60 with D100's system and asking port in both address
 * fields, XMCSM 0x0100, and 101 = local, detail 101, distance 0 (the shape of every captured
 * reply a machine gives about itself, e.g. ListRoutingTests.cs CapturedResponseHex). The
 * checksum DCED is the ones-complement sum of words 0-5 (sintran_hdr.h). */
#define EXPECTED_REPLY                                                                             \
    "21 13 00 0E 00 64 00 65 00 28 01 00 DC ED 21 00 86 60 00 64 09 D4 00 64 09 D4 01 00 "        \
    "01 00 00 10 01 02 00 65 02 02 00 04 03 02 00 65 04 02 00 00"

/* ------------------------------------------------------------------ Flags1 store */

typedef struct Store
{
    uint16_t node[4];
    uint16_t value[4];
    bool has[4];
} Store;

static int store_load(void *ctx, uint16_t node, uint16_t *next)
{
    Store *s = (Store *)ctx;
    for (int i = 0; i < 4; i++)
    {
        if (s->has[i] && s->node[i] == node)
        {
            *next = s->value[i];
            return 0;
        }
    }
    return -1;
}

static int store_save(void *ctx, uint16_t node, uint16_t next)
{
    Store *s = (Store *)ctx;
    for (int i = 0; i < 4; i++)
    {
        if (!s->has[i] || s->node[i] == node)
        {
            s->has[i] = true;
            s->node[i] = node;
            s->value[i] = next;
            return 0;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ nodes on the pair */

static XmsgNode s_a; /* 101, us */
static XmsgNode s_b; /* 100, the host */
static TestPair s_pair;
static TestPairPort s_port_a;
static TestPairPort s_port_b;
static Store s_store_a;
static Store s_store_b;

/* What B (the host) caught: datagrams no session claimed, and port-0 letters. */
static uint8_t s_caught[8][64];
static size_t s_caught_len[8];
static bool s_caught_repeat[8];
static int s_caught_count;
static int s_letters_b;

static void b_unhandled(void *ctx, const XmsgDatagram *dg)
{
    (void)ctx;
    if (s_caught_count < 8 && dg->has_subhdr && dg->body_len + XMSG_DG_HDR_SIZE <= 64)
    {
        /* The body points into the received info field, 28 bytes after its start. */
        memcpy(s_caught[s_caught_count], dg->body - XMSG_DG_HDR_SIZE,
               dg->body_len + XMSG_DG_HDR_SIZE);
        s_caught_len[s_caught_count] = dg->body_len + XMSG_DG_HDR_SIZE;
        s_caught_repeat[s_caught_count] = dg->repeat;
        s_caught_count++;
    }
}

static void b_on_letter(void *ctx, const XmsgDatagram *dg, uint32_t now_ms)
{
    (void)ctx;
    (void)dg;
    (void)now_ms;
    s_letters_b++;
}

static void node_init(XmsgNode *n, uint16_t node, Store *store, XmsgTransportPort *tp,
                      bool answer_routing)
{
    XmsgNodeConfig cfg;
    XmsgPorts ports;
    XmsgNodeCallbacks cb;
    xmsg_node_config_default(&cfg, node);
    cfg.lapb.t1_ms = 50;
    cfg.lapb.t3_ms = 2000;
    cfg.settle_ms = 20;
    cfg.answer_routing = answer_routing;
    memset(&ports, 0, sizeof ports);
    ports.transport = *tp;
    ports.flags1.ctx = store;
    ports.flags1.load = store_load;
    ports.flags1.save = store_save;
    memset(&cb, 0, sizeof cb);
    if (node == 100)
    {
        cb.unhandled = b_unhandled;
        cb.on_letter = b_on_letter;
    }
    CHECK_EQ(xmsg_node_init(n, &cfg, &ports, &cb), XMSG_OK);
}

static void pump_pair(uint32_t now)
{
    uint8_t chunk[256];
    size_t n;
    xmsg_node_tick(&s_a, now);
    xmsg_node_tick(&s_b, now);
    while ((n = test_pair_read(&s_pair, 0, chunk, sizeof chunk)) > 0)
    {
        xmsg_node_feed(&s_a, chunk, n, now);
    }
    while ((n = test_pair_read(&s_pair, 1, chunk, sizeof chunk)) > 0)
    {
        xmsg_node_feed(&s_b, chunk, n, now);
    }
}

static uint32_t run(uint32_t now, unsigned ms)
{
    for (unsigned i = 0; i < ms; i++)
    {
        now++;
        pump_pair(now);
    }
    return now;
}

static uint32_t bring_up(bool b_answers)
{
    XmsgTransportPort ta;
    XmsgTransportPort tb;
    uint32_t now = 0;
    memset(&s_store_a, 0, sizeof s_store_a);
    memset(&s_store_b, 0, sizeof s_store_b);
    /* Our next number to D100 was 0x0028 when the live request came (frame 4899 + 1). */
    (void)store_save(&s_store_a, 100, 0x0028);
    s_caught_count = 0;
    s_letters_b = 0;
    test_pair_init(&s_pair);
    test_pair_transport(&s_pair, 0, &s_port_a, &ta);
    test_pair_transport(&s_pair, 1, &s_port_b, &tb);
    node_init(&s_a, 101, &s_store_a, &ta, true);
    node_init(&s_b, 100, &s_store_b, &tb, b_answers);
    xmsg_node_link_start(&s_a, now);
    xmsg_node_link_start(&s_b, now);
    while (!(xmsg_node_is_ready(&s_a) && xmsg_node_is_ready(&s_b)) && now < 5000)
    {
        now++;
        pump_pair(now);
    }
    CHECK(xmsg_node_is_ready(&s_a));
    CHECK(xmsg_node_is_ready(&s_b));
    return now;
}

/* Put raw info bytes into B's window, as the host would send them. */
static void host_sends(const uint8_t *info, size_t len, uint32_t now)
{
    uint8_t *slot;
    size_t cap;
    CHECK_EQ(lapb_tx_reserve(&s_b.lapb, &slot, &cap), XMSG_OK);
    if (cap < len)
    {
        CHECK(false);
        return;
    }
    memcpy(slot, info, len);
    CHECK_EQ(lapb_tx_commit(&s_b.lapb, len, now), XMSG_OK);
}

/* A host request asking for `asked` at Flags1 `f1`, otherwise frame 4905. */
static size_t host_request(uint8_t *out, size_t cap, uint16_t f1, uint16_t asked)
{
    uint8_t body[XROUT_ROUTE_REQ_SIZE];
    XmsgSubhdr sub = {0x86, 0x84, 101, 0x0000, 100, 0x09D4, XROUT_ROUTE_XMCSM};
    xrout_route_request_write(body, asked);
    return xmsg_dg_write_data(out, cap, 0x0E, 101, 100, f1, XROUT_ROUTE_XMCSM, &sub, body,
                              sizeof body);
}

/* ------------------------------------------------------------------ the answer side */

static void node_answers_the_live_request(void)
{
    uint8_t live[64];
    uint8_t req[64];
    XroutRouteEntry e;
    uint8_t status = 0;
    XmsgDatagram dg;
    int n = test_hex_to_bytes(LIVE_D100_REQUEST, live, sizeof live);
    CHECK_EQ(n, 36); /* 14 header + 14 sub-header + 8 body; the log line adds LAPB and FCS */
    uint32_t now = bring_up(true);
    uint32_t acks_before = s_b.stats.acks_received;

    /* The request that hung li-rout on D100. */
    host_sends(live, (size_t)n, now);
    now = run(now, 300);
    CHECK_EQ(s_a.stats.route_requests, 1);
    CHECK_EQ(s_a.stats.route_replies, 1);
    CHECK_EQ(s_caught_count, 1);
    CHECK_BYTES(s_caught[0], s_caught_len[0], EXPECTED_REPLY);
    /* Acknowledged like every data frame, and the reply spent exactly one number of ours. */
    CHECK_EQ(s_b.stats.acks_received - acks_before, 1);
    CHECK_EQ(xmsg_flags1_peek(&s_a.flags1, 100), 0x0029);

    /* The same request again (a retransmission, same Flags1): the same reply at the same
     * number, and no new number spent. */
    host_sends(live, (size_t)n, now);
    now = run(now, 300);
    CHECK_EQ(s_a.stats.repeats, 1);
    CHECK_EQ(s_a.stats.route_replies, 2);
    CHECK_EQ(s_caught_count, 2);
    CHECK_BYTES(s_caught[1], s_caught_len[1], EXPECTED_REPLY);
    CHECK_EQ(xmsg_flags1_peek(&s_a.flags1, 100), 0x0029);

    /* The host's second question about the same system (it sends two, XMSG-PROTOCOL.md 9.1.1)
     * is a new datagram: a new reply on the next number of ours. */
    size_t rl = host_request(req, sizeof req, 0x002A, 101);
    host_sends(req, rl, now);
    now = run(now, 300);
    CHECK_EQ(s_caught_count, 3);
    CHECK_EQ(xmsg_dg_parse(s_caught[2], s_caught_len[2], &dg), XMSG_OK);
    CHECK_EQ(dg.hdr.flags1, 0x0029);
    CHECK_EQ(xrout_route_reply_read(dg.body, dg.body_len, &status, &e), XMSG_OK);
    CHECK_EQ(e.system, 101);
    CHECK_EQ(e.how, XROUT_REACH_LOCAL);

    /* Asked about the host itself: we reach it directly, on link 1, one hop. */
    rl = host_request(req, sizeof req, 0x002B, 100);
    host_sends(req, rl, now);
    now = run(now, 300);
    CHECK_EQ(s_caught_count, 4);
    CHECK_EQ(xmsg_dg_parse(s_caught[3], s_caught_len[3], &dg), XMSG_OK);
    CHECK_EQ(dg.hdr.flags1, 0x002A);
    CHECK_EQ(xrout_route_reply_read(dg.body, dg.body_len, &status, &e), XMSG_OK);
    CHECK_EQ(e.system, 100);
    CHECK_EQ(e.how, XROUT_REACH_NEIGHBOUR);
    CHECK_EQ(e.detail, 1);
    CHECK_EQ(e.distance, 1);

    /* Past the end of our table (100, 101): system 0, all zeros, status still OK. */
    rl = host_request(req, sizeof req, 0x002C, 102);
    host_sends(req, rl, now);
    (void)run(now, 300);
    CHECK_EQ(s_caught_count, 5);
    CHECK_EQ(xmsg_dg_parse(s_caught[4], s_caught_len[4], &dg), XMSG_OK);
    CHECK_BYTES(dg.body, dg.body_len, "0100001001020000020200000302000004020000");
    CHECK_EQ(s_a.stats.route_bad, 0);
    CHECK_EQ(s_a.stats.unhandled, 0); /* none of it went to the application */
}

/* ------------------------------------------------------------------ the walk */

typedef struct WalkResult
{
    XroutRouteEntry e[16];
    int count;
    bool done;
    int result;
    unsigned entries;
} WalkResult;

static void on_entry(void *ctx, const XroutRouteEntry *e)
{
    WalkResult *r = (WalkResult *)ctx;
    if (r->count < 16)
    {
        r->e[r->count++] = *e;
    }
}

static void on_done(void *ctx, int result, unsigned entries)
{
    WalkResult *r = (WalkResult *)ctx;
    r->done = true;
    r->result = result;
    r->entries = entries;
}

static void walk_reads_the_whole_table(void)
{
    static XroutWalk w;
    WalkResult r;
    XroutRouteEntry add;
    uint32_t now = bring_up(true);
    memset(&r, 0, sizeof r);
    memset(&w, 0, sizeof w);

    /* The host's table: itself and us appear on their own; two more routes are configured. */
    add = (XroutRouteEntry){102, XROUT_REACH_NEIGHBOUR, 2, 1};
    CHECK_EQ(xmsg_node_route_put(&s_b, &add), XMSG_OK);
    add = (XroutRouteEntry){200, XROUT_REACH_VIA, 102, 2};
    CHECK_EQ(xmsg_node_route_put(&s_b, &add), XMSG_OK);

    XroutWalkCallbacks cb = {&r, on_entry, on_done};
    uint16_t f1_before = xmsg_flags1_peek(&s_a.flags1, 100);
    CHECK_EQ(xrout_walk_start(&w, &s_a, 100, 100, 2000, &cb, now), XMSG_OK);
    CHECK(xrout_walk_is_busy(&w));
    CHECK_EQ(xrout_walk_start(&w, &s_a, 100, 100, 2000, &cb, now), XMSG_EAGAIN);
    for (int i = 0; i < 5000 && !r.done; i++)
    {
        now++;
        pump_pair(now);
    }
    CHECK(r.done);
    CHECK_EQ(r.result, XMSG_OK);
    CHECK_EQ(r.entries, 4);
    CHECK_EQ(r.count, 4);
    CHECK_EQ(r.e[0].system, 100);
    CHECK_EQ(r.e[0].how, XROUT_REACH_LOCAL);
    CHECK_EQ(r.e[0].detail, 100);
    CHECK_EQ(r.e[1].system, 101);
    CHECK_EQ(r.e[1].how, XROUT_REACH_NEIGHBOUR);
    CHECK_EQ(r.e[1].detail, 1);
    CHECK_EQ(r.e[1].distance, 1);
    CHECK_EQ(r.e[2].system, 102);
    CHECK_EQ(r.e[2].detail, 2);
    CHECK_EQ(r.e[3].system, 200);
    CHECK_EQ(r.e[3].how, XROUT_REACH_VIA);
    CHECK_EQ(r.e[3].detail, 102);
    CHECK_EQ(r.e[3].distance, 2);
    /* Five questions (1, 101, 102, 103, 201), each its own number of ours. */
    CHECK_EQ(xmsg_flags1_peek(&s_a.flags1, 100), (uint16_t)(f1_before + 5u));
    CHECK_EQ(s_b.stats.route_requests, 5);
    CHECK_EQ(s_b.stats.route_replies, 5);
    CHECK(!xrout_walk_is_busy(&w));
    CHECK(!w.base.attached); /* the session slot is free again */

    /* A second walk can start at once (on_done had freed the slot). */
    memset(&r, 0, sizeof r);
    CHECK_EQ(xrout_walk_start(&w, &s_a, 100, 100, 2000, &cb, now), XMSG_OK);
    for (int i = 0; i < 5000 && !r.done; i++)
    {
        now++;
        pump_pair(now);
    }
    CHECK_EQ(r.result, XMSG_OK);
    CHECK_EQ(r.entries, 4);
}

static void walk_times_out_when_the_host_is_silent(void)
{
    static XroutWalk w;
    WalkResult r;
    uint32_t now = bring_up(false); /* B acknowledges but does not answer, as libxmsg did */
    memset(&r, 0, sizeof r);
    memset(&w, 0, sizeof w);
    XroutWalkCallbacks cb = {&r, on_entry, on_done};
    CHECK_EQ(xrout_walk_start(&w, &s_a, 100, 100, 500, &cb, now), XMSG_OK);
    uint32_t start = now;
    for (int i = 0; i < 3000 && !r.done; i++)
    {
        now++;
        pump_pair(now);
    }
    CHECK(r.done);
    CHECK_EQ(r.result, XMSG_ETIMEDOUT);
    CHECK_EQ(r.entries, 0);
    CHECK(now - start >= 500);
    CHECK(now - start < 600);
    CHECK_EQ(s_letters_b, 1); /* the question reached B and went to its application */
    CHECK(!w.base.attached);
}

static void walk_needs_a_ready_link(void)
{
    static XroutWalk w;
    static XmsgNode lone;
    XmsgNodeConfig cfg;
    XmsgPorts ports;
    XmsgTransportPort tp;
    WalkResult r;
    TestPair p;
    TestPairPort pp;
    test_pair_init(&p);
    test_pair_transport(&p, 0, &pp, &tp);
    xmsg_node_config_default(&cfg, 101);
    memset(&ports, 0, sizeof ports);
    ports.transport = tp;
    CHECK_EQ(xmsg_node_init(&lone, &cfg, &ports, NULL), XMSG_OK);
    memset(&w, 0, sizeof w);
    XroutWalkCallbacks cb = {&r, on_entry, on_done};
    CHECK_EQ(xrout_walk_start(&w, &lone, 100, 100, 500, &cb, 0), XMSG_ESTATE);
    CHECK_EQ(xrout_walk_start(NULL, &lone, 100, 100, 500, &cb, 0), XMSG_EINVAL);
    CHECK_EQ(xrout_walk_cancel(&w), XMSG_ESTATE);
}

int main(void)
{
    printf("[pair_route]\n");
    node_answers_the_live_request();
    walk_reads_the_whole_table();
    walk_times_out_when_the_host_is_silent();
    walk_needs_a_ready_link();
    TEST_DONE();
}
