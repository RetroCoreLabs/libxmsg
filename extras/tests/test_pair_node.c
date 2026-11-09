/*
 * test_pair_node.c - two nodes talk: link up, settle, datagrams both ways with acknowledgements,
 * fragments, repeats, the reachability handshake, and a sequence that survives a restart.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Node A (100) and node B (102) sit on the in-memory pair, then on a loopback TCP socket. Each
 * carries one test session that sends numbered bodies of growing size (so some are fragmented)
 * and checks what arrives. This is the first test of the whole stack below the protocols.
 */
#include "libxmsg/xmsg/xmsg_node.h"

#include "pair/test_pair.h"
#include "test_util.h"

#define MESSAGES 120

/* ------------------------------------------------------------------ fake Flags1 store */

typedef struct Store
{
    uint16_t node[4];
    uint16_t value[4];
    bool has[4];
    int saves;
    uint16_t port_seed;
    bool has_port_seed;
} Store;

static int store_load_seed(void *ctx, uint16_t *seed)
{
    Store *s = (Store *)ctx;
    if (!s->has_port_seed)
    {
        return -1;
    }
    *seed = s->port_seed;
    return 0;
}

static int store_save_seed(void *ctx, uint16_t seed)
{
    Store *s = (Store *)ctx;
    s->port_seed = seed;
    s->has_port_seed = true;
    return 0;
}

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
    s->saves++;
    for (int i = 0; i < 4; i++)
    {
        if (s->has[i] && s->node[i] == node)
        {
            s->value[i] = next;
            return 0;
        }
    }
    for (int i = 0; i < 4; i++)
    {
        if (!s->has[i])
        {
            s->has[i] = true;
            s->node[i] = node;
            s->value[i] = next;
            return 0;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ the test session */

typedef struct TestSession
{
    XmsgSession base; /* first */
    unsigned next_send;
    unsigned next_expected;
    unsigned received;
    unsigned repeats;
    unsigned misordered;
    unsigned bad_bodies;
    unsigned delivered; /* ACKs seen for our frames */
    unsigned errors;
    unsigned restarts; /* reachability requests from our peer */
    unsigned limit;    /* how many to send */
    uint16_t last_flags1;
} TestSession;

static size_t body_for(unsigned seq, uint8_t *out)
{
    size_t len = 2 + (seq * 97) % 1100; /* 2..1101: well past one fragment */
    out[0] = (uint8_t)(seq >> 8);
    out[1] = (uint8_t)seq;
    for (size_t i = 2; i < len; i++)
    {
        out[i] = (uint8_t)(seq + i * 3);
    }
    return len;
}

static void ts_on_datagram(XmsgSession *s, const XmsgDatagram *dg, uint32_t now_ms)
{
    TestSession *t = (TestSession *)s;
    (void)now_ms;
    if (dg->repeat)
    {
        t->repeats++;
        return;
    }
    if (dg->body_len < 2)
    {
        t->bad_bodies++;
        return;
    }
    unsigned seq = ((unsigned)dg->body[0] << 8) | dg->body[1];
    uint8_t expect[1200];
    size_t el = body_for(seq, expect);
    if (el != dg->body_len || memcmp(expect, dg->body, el) != 0)
    {
        t->bad_bodies++;
    }
    if (seq != t->next_expected)
    {
        t->misordered++;
    }
    t->next_expected = seq + 1;
    t->received++;
}

static int ts_poll_tx(XmsgSession *s, uint32_t now_ms)
{
    TestSession *t = (TestSession *)s;
    uint8_t *body;
    size_t cap;
    if (t->next_send >= t->limit)
    {
        s->wants_tx = false;
        return 0;
    }
    /* Lock-step like the real protocols (file access waits for each reply, TAD for RFI): at most
     * four datagrams unacknowledged, so the peer never has to queue more ACKs than that. */
    if (t->next_send - t->delivered >= 4)
    {
        return 0;
    }
    if (xmsg_node_send_begin(s->node, &body, &cap) != XMSG_OK)
    {
        return 0;
    }
    size_t len = body_for(t->next_send, body);
    XmsgSubhdr sub = {0x96, 0x84, s->peer_system, s->peer_port, 0, s->local_port, (uint16_t)len};
    int rc = xmsg_node_send_commit(s->node, s->peer_node, &sub, len, now_ms, &t->last_flags1);
    if (rc == XMSG_EAGAIN)
    {
        xmsg_node_send_cancel(s->node);
        return 0;
    }
    CHECK_EQ(rc, XMSG_OK);
    t->next_send++;
    return 1;
}

static void ts_on_delivered(XmsgSession *s, uint16_t flags1)
{
    (void)flags1;
    ((TestSession *)s)->delivered++;
}

static void ts_on_error(XmsgSession *s, int16_t code, uint16_t flags1)
{
    (void)code;
    (void)flags1;
    ((TestSession *)s)->errors++;
}

static void ts_on_peer_restart(XmsgSession *s)
{
    ((TestSession *)s)->restarts++;
}

static const XmsgSessionOps TS_OPS = {ts_on_datagram,  ts_poll_tx,  NULL,
                                      ts_on_delivered, ts_on_error, ts_on_peer_restart};

/* ------------------------------------------------------------------ nodes on the pair */

static XmsgNode s_a;
static XmsgNode s_b;
static TestPair s_pair;
static TestPairPort s_port_a;
static TestPairPort s_port_b;
static Store s_store_a;
static Store s_store_b;
static TestSession s_sess_a;
static TestSession s_sess_b;
static int s_reach_seen_b;

static void on_peer_reach_b(void *ctx, uint16_t peer)
{
    (void)ctx;
    (void)peer;
    s_reach_seen_b++;
}

static void node_init(XmsgNode *n, uint16_t node, Store *store, XmsgTransportPort *tp,
                      void (*reach)(void *, uint16_t))
{
    XmsgNodeConfig cfg;
    XmsgPorts ports;
    XmsgNodeCallbacks cb;
    xmsg_node_config_default(&cfg, node);
    cfg.lapb.t1_ms = 50;
    cfg.lapb.t3_ms = 2000;
    cfg.settle_ms = 20;
    memset(&ports, 0, sizeof ports);
    ports.transport = *tp;
    ports.flags1.ctx = store;
    ports.flags1.load = store_load;
    ports.flags1.save = store_save;
    ports.flags1.load_port_seed = store_load_seed;
    ports.flags1.save_port_seed = store_save_seed;
    memset(&cb, 0, sizeof cb);
    cb.peer_reach = reach;
    CHECK_EQ(xmsg_node_init(n, &cfg, &ports, &cb), XMSG_OK);
}

static void session_init(TestSession *t, XmsgNode *n, uint16_t peer_node, unsigned limit)
{
    memset(t, 0, sizeof *t);
    t->base.ops = &TS_OPS;
    t->base.peer_node = peer_node;
    t->base.peer_system = peer_node;
    t->limit = limit;
    CHECK_EQ(xmsg_node_attach(n, &t->base), XMSG_OK);
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

static uint32_t bring_up(uint32_t now, const TestFaults *fa, const TestFaults *fb)
{
    XmsgTransportPort ta;
    XmsgTransportPort tb;
    test_pair_init(&s_pair);
    test_pair_set_faults(&s_pair, 0, fa);
    test_pair_set_faults(&s_pair, 1, fb);
    test_pair_transport(&s_pair, 0, &s_port_a, &ta);
    test_pair_transport(&s_pair, 1, &s_port_b, &tb);
    node_init(&s_a, 100, &s_store_a, &ta, NULL);
    node_init(&s_b, 102, &s_store_b, &tb, on_peer_reach_b);
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

static void run_traffic(const TestFaults *fa, const TestFaults *fb, const char *label)
{
    memset(&s_store_a, 0, sizeof s_store_a);
    memset(&s_store_b, 0, sizeof s_store_b);
    uint32_t now = bring_up(0, fa, fb);
    session_init(&s_sess_a, &s_a, 102, MESSAGES);
    session_init(&s_sess_b, &s_b, 100, MESSAGES);
    /* Each side learns the other's port "out of band" here; a letter does it for real. */
    s_sess_a.base.peer_port = s_sess_b.base.local_port;
    s_sess_b.base.peer_port = s_sess_a.base.local_port;
    s_sess_a.base.wants_tx = true;
    s_sess_b.base.wants_tx = true;

    uint32_t start = now;
    while ((s_sess_a.received < MESSAGES || s_sess_b.received < MESSAGES ||
            s_sess_a.delivered < MESSAGES || s_sess_b.delivered < MESSAGES) &&
           now - start < 120000)
    {
        now++;
        pump_pair(now);
    }
    CHECK_EQ(s_sess_a.received, MESSAGES);
    CHECK_EQ(s_sess_b.received, MESSAGES);
    /* Every datagram is acknowledged unless the receiver's acknowledgement queue overflowed
     * while its own window was stuck behind lost frames; those drops are counted there, and the
     * sender heard about exactly the rest. (XMSG retransmits the unacknowledged datagram in real
     * traffic; the sessions here do not, so the count is what proves the accounting.) */
    CHECK_EQ(s_sess_a.delivered + s_b.stats.short_queue_full, MESSAGES);
    CHECK_EQ(s_sess_b.delivered + s_a.stats.short_queue_full, MESSAGES);
    CHECK_EQ(s_sess_a.misordered, 0);
    CHECK_EQ(s_sess_b.misordered, 0);
    CHECK_EQ(s_sess_a.bad_bodies, 0);
    CHECK_EQ(s_sess_b.bad_bodies, 0);
    CHECK_EQ(s_sess_a.repeats, 0);
    CHECK_EQ(s_sess_b.repeats, 0);
    CHECK_EQ(s_sess_a.errors, 0);
    CHECK_EQ(s_sess_b.errors, 0);
    CHECK(s_a.stats.bad_headers == 0 && s_b.stats.bad_headers == 0);
    CHECK(s_b.reasm.joined > 0); /* bodies over 594 bytes were fragmented and joined */
    CHECK_EQ(s_b.reasm.orphaned, 0);
    /* Every datagram was acknowledged: ACKs out equal datagrams in, both sides. */
    /* A fragmented message is acknowledged per fragment, so ACKs = messages + joined pairs. */
    CHECK_EQ(s_a.stats.acks_received + s_b.stats.short_queue_full, MESSAGES + s_a.reasm.joined);
    CHECK_EQ(s_b.stats.acks_received + s_a.stats.short_queue_full, MESSAGES + s_b.reasm.joined);
    printf("  counters a: short_full %u frmr_sent %u frmr_rcvd %u bad_hdr %u unhandled %u | b: "
           "short_full %u frmr_sent %u frmr_rcvd %u bad_hdr %u unhandled %u\n",
           s_a.stats.short_queue_full, s_a.lapb.stats.frmr_sent, s_a.lapb.stats.frmr_received,
           s_a.stats.bad_headers, s_a.stats.unhandled, s_b.stats.short_queue_full,
           s_b.lapb.stats.frmr_sent, s_b.lapb.stats.frmr_received, s_b.stats.bad_headers,
           s_b.stats.unhandled);
    /* Flags1 ran 0..MESSAGES-1 on both sides and the stores say so. */
    CHECK_EQ(xmsg_flags1_peek(&s_a.flags1, 102), MESSAGES);
    CHECK_EQ(xmsg_flags1_peek(&s_b.flags1, 100), MESSAGES);
    uint16_t stored = 0;
    CHECK_EQ(store_load(&s_store_a, 102, &stored), 0);
    CHECK_EQ(stored, MESSAGES);
    if (fa == NULL && fb == NULL)
    {
        CHECK_EQ(s_a.stats.short_queue_full + s_b.stats.short_queue_full, 0);
    }
    printf("  %s: %u ms, %u msgs each way, %u joined at b, lapb retx a %u b %u\n", label,
           (unsigned)(now - start), MESSAGES, s_b.reasm.joined, s_a.lapb.stats.retransmissions,
           s_b.lapb.stats.retransmissions);
}

static void clean_wire_traffic(void)
{
    run_traffic(NULL, NULL, "clean wire");
}

static void faulty_wire_traffic(void)
{
    TestFaults fa = {9, 0, 11, 5, false};
    TestFaults fb = {0, 7, 13, 3, false};
    run_traffic(&fa, &fb, "faulty wire");
}

static void repeat_is_flagged_and_restart_continues_the_sequence(void)
{
    memset(&s_store_a, 0, sizeof s_store_a);
    memset(&s_store_b, 0, sizeof s_store_b);
    uint32_t now = bring_up(0, NULL, NULL);
    session_init(&s_sess_a, &s_a, 102, 3);
    session_init(&s_sess_b, &s_b, 100, 0);
    s_sess_a.base.peer_port = s_sess_b.base.local_port;
    s_sess_b.base.peer_port = s_sess_a.base.local_port;
    s_sess_a.base.wants_tx = true;
    uint32_t start = now;
    while (s_sess_b.received < 3 && now - start < 5000)
    {
        now++;
        pump_pair(now);
    }
    CHECK_EQ(s_sess_b.received, 3);
    CHECK_EQ(s_sess_a.last_flags1, 2);
    /* Let the acknowledgements drain so the window is free again. */
    for (int i = 0; i < 200; i++)
    {
        now++;
        pump_pair(now);
    }
    CHECK_EQ(s_sess_a.delivered, 3);

    /* A repeat of Flags1 2 at the datagram level (as a sender retransmitting): B flags it. */
    uint8_t *body = NULL;
    size_t cap = 0;
    CHECK_EQ(xmsg_node_send_begin(&s_a, &body, &cap), XMSG_OK);
    if (body == NULL)
    {
        return;
    }
    size_t len = body_for(2, body);
    XmsgSubhdr sub = {
        0x96, 0x84, 102, s_sess_b.base.local_port, 0, s_sess_a.base.local_port, (uint16_t)len};
    CHECK_EQ(xmsg_node_send_repeat(&s_a, 102, &sub, len, 2, now), XMSG_OK);
    for (int i = 0; i < 200; i++)
    {
        now++;
        pump_pair(now);
    }
    CHECK_EQ(s_sess_b.repeats, 1);
    CHECK_EQ(s_sess_b.received, 3);
    CHECK_EQ(s_b.stats.repeats, 1);

    /* Reachability: A announces a restart; B zeroes its count to A and replies. */
    CHECK_EQ(xmsg_node_send_reach(&s_a, 102, now), XMSG_OK);
    for (int i = 0; i < 200; i++)
    {
        now++;
        pump_pair(now);
    }
    CHECK_EQ(s_reach_seen_b, 1);
    CHECK_EQ(s_sess_b.restarts, 1); /* B's session on A is told */
    CHECK_EQ(s_sess_a.restarts, 0); /* the reply restarts nothing */
    CHECK_EQ(xmsg_flags1_peek(&s_b.flags1, 100), 0);
    CHECK_EQ(xmsg_flags1_peek(&s_a.flags1, 102), 3); /* our own request touches nothing */

    /* "Restart" node A over the same store: its next number to B is 3, not 0. */
    XmsgTransportPort ta;
    test_pair_transport(&s_pair, 0, &s_port_a, &ta);
    uint16_t old_port = s_sess_a.base.local_port;
    node_init(&s_a, 100, &s_store_a, &ta, NULL);
    CHECK_EQ(xmsg_flags1_peek(&s_a.flags1, 102), 3);
    /* The port-word draws continue too: the same port index gets a NEW word after the restart,
     * so frames the peer still sends to the old session do not reach the new one. */
    CHECK(s_store_a.has_port_seed);
    TestSession again;
    session_init(&again, &s_a, 102, 0);
    CHECK_EQ(again.base.local_port >> 7, old_port >> 7);
    CHECK(again.base.local_port != old_port);
}

static void nothing_is_sent_before_the_link_settles(void)
{
    XmsgTransportPort ta;
    XmsgTransportPort tb;
    test_pair_init(&s_pair);
    test_pair_transport(&s_pair, 0, &s_port_a, &ta);
    test_pair_transport(&s_pair, 1, &s_port_b, &tb);
    node_init(&s_a, 100, &s_store_a, &ta, NULL);
    node_init(&s_b, 102, &s_store_b, &tb, NULL);
    uint8_t *body;
    size_t cap;
    CHECK_EQ(xmsg_node_send_begin(&s_a, &body, &cap), XMSG_ESTATE);
    uint32_t now = 0;
    xmsg_node_link_start(&s_a, now);
    xmsg_node_link_start(&s_b, now);
    while (!xmsg_node_is_up(&s_a) && now < 1000)
    {
        now++;
        pump_pair(now);
    }
    CHECK(xmsg_node_is_up(&s_a));
    CHECK(!xmsg_node_is_ready(&s_a));
    CHECK_EQ(xmsg_node_send_begin(&s_a, &body, &cap), XMSG_ESTATE);
    for (int i = 0; i < 40; i++)
    {
        now++;
        pump_pair(now);
    }
    CHECK(xmsg_node_is_ready(&s_a));
    CHECK_EQ(xmsg_node_send_begin(&s_a, &body, &cap), XMSG_OK);
    xmsg_node_send_cancel(&s_a);
    /* A polite stop takes both down. */
    xmsg_node_link_stop(&s_a, now);
    for (int i = 0; i < 100; i++)
    {
        now++;
        pump_pair(now);
    }
    CHECK(!xmsg_node_is_up(&s_a));
    CHECK(!xmsg_node_is_up(&s_b));
}

/* ------------------------------------------------------------------ over loopback TCP */

static TestTcpPair s_tcp;
static TestTcpPort s_tcp_port_a;
static TestTcpPort s_tcp_port_b;

static void over_loopback_tcp(void)
{
    XmsgTransportPort ta;
    XmsgTransportPort tb;
    if (test_tcp_pair_open(&s_tcp) != 0)
    {
        CHECK(false);
        return;
    }
    test_tcp_transport(s_tcp.sock[0], &s_tcp_port_a, &ta);
    test_tcp_transport(s_tcp.sock[1], &s_tcp_port_b, &tb);
    memset(&s_store_a, 0, sizeof s_store_a);
    memset(&s_store_b, 0, sizeof s_store_b);
    node_init(&s_a, 100, &s_store_a, &ta, NULL);
    node_init(&s_b, 102, &s_store_b, &tb, NULL);
    s_a.cfg.lapb.t1_ms = 300;
    s_b.cfg.lapb.t1_ms = 300;
    uint32_t start = test_now_ms();
    uint32_t now = start;
    xmsg_node_link_start(&s_a, now);
    xmsg_node_link_start(&s_b, now);
    session_init(&s_sess_a, &s_a, 102, MESSAGES);
    session_init(&s_sess_b, &s_b, 100, MESSAGES);
    s_sess_a.base.peer_port = s_sess_b.base.local_port;
    s_sess_b.base.peer_port = s_sess_a.base.local_port;
    s_sess_a.base.wants_tx = true;
    s_sess_b.base.wants_tx = true;
    uint8_t chunk[1024];
    while ((s_sess_a.received < MESSAGES || s_sess_b.received < MESSAGES) && now - start < 20000)
    {
        now = test_now_ms();
        xmsg_node_tick(&s_a, now);
        xmsg_node_tick(&s_b, now);
        int r;
        while ((r = test_tcp_read(s_tcp.sock[0], chunk, sizeof chunk)) > 0)
        {
            xmsg_node_feed(&s_a, chunk, (size_t)r, now);
        }
        while ((r = test_tcp_read(s_tcp.sock[1], chunk, sizeof chunk)) > 0)
        {
            xmsg_node_feed(&s_b, chunk, (size_t)r, now);
        }
        if (!test_tcp_wait_readable(s_tcp.sock[0], 1))
        {
            test_tcp_wait_readable(s_tcp.sock[1], 1);
        }
    }
    CHECK_EQ(s_sess_a.received, MESSAGES);
    CHECK_EQ(s_sess_b.received, MESSAGES);
    CHECK_EQ(s_sess_a.misordered + s_sess_b.misordered, 0);
    CHECK_EQ(s_sess_a.bad_bodies + s_sess_b.bad_bodies, 0);
    printf("  tcp loopback: %u ms for %u messages each way\n", (unsigned)(now - start), MESSAGES);
    test_tcp_pair_close(&s_tcp);
}

int main(void)
{
    printf("[pair_node]\n");
    clean_wire_traffic();
    faulty_wire_traffic();
    repeat_is_flagged_and_restart_continues_the_sequence();
    nothing_is_sent_before_the_link_settles();
    over_loopback_tcp();
    TEST_DONE();
}
