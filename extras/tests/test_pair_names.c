/*
 * test_pair_names.c - name questions end to end: our node answers LIST-NAMES (XSGNI) exactly as
 *                     D100 did, and our walk reads a remote name table, over the in-memory pair.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * The vectors are the complete walk captured 2026-11-10 on the COSMOS Ethernet hub (TCP 5010,
 * listnames-hub.pcapng): D102 ran LIST-NAMES against D100 and read all 13 rows. Here node A is
 * 100 and answers, from a table built to match D100's. Node B is 102 and plays the asker: it puts
 * the EXACT 14 captured requests into its LAPB window one at a time and catches A's replies as
 * unclaimed datagrams. A's count to 102 is set to 0x0011, D100's number for its first reply, so
 * every reply must equal the captured datagram byte for byte - header, checksum and body.
 */
#include "libxmsg/xmsg/xmsg_node.h"
#include "libxmsg/xrout/xrout_names_walk.h"

#include "pair/test_pair.h"
#include "test_util.h"

#define WALK_STEPS 14

/* Requests 102 -> 100, frames #27, #75, ... #651; 102's count 0x0012 .. 0x001F. */
static const char *const CAPTURED_REQ[WALK_STEPS] = {
    "21 13 00 0E 00 64 00 66 00 12 01 00 DD 02 21 00 86 84 00 64 00 00 00 66 06 B2 01 00 "
    "01 45 00 04 01 02 00 00",
    "21 13 00 0E 00 64 00 66 00 13 01 00 DD 01 21 00 86 84 00 64 00 00 00 66 06 B2 01 00 "
    "01 45 00 06 01 04 00 64 00 02",
    "21 13 00 0E 00 64 00 66 00 14 01 00 DD 00 21 00 86 84 00 64 00 00 00 66 06 B2 01 00 "
    "01 45 00 06 01 04 00 64 01 DB",
    "21 13 00 0E 00 64 00 66 00 15 01 00 DC FF 21 00 86 84 00 64 00 00 00 66 06 B2 01 00 "
    "01 45 00 06 01 04 00 64 02 28",
    "21 13 00 0E 00 64 00 66 00 16 01 00 DC FE 21 00 86 84 00 64 00 00 00 66 06 B2 01 00 "
    "01 45 00 06 01 04 00 64 02 D1",
    "21 13 00 0E 00 64 00 66 00 17 01 00 DC FD 21 00 86 84 00 64 00 00 00 66 06 B2 01 00 "
    "01 45 00 06 01 04 00 64 03 AD",
    "21 13 00 0E 00 64 00 66 00 18 01 00 DC FC 21 00 86 84 00 64 00 00 00 66 06 B2 01 00 "
    "01 45 00 06 01 04 00 64 04 02",
    "21 13 00 0E 00 64 00 66 00 19 01 00 DC FB 21 00 86 84 00 64 00 00 00 66 06 B2 01 00 "
    "01 45 00 06 01 04 00 64 04 CF",
    "21 13 00 0E 00 64 00 66 00 1A 01 00 DC FA 21 00 86 84 00 64 00 00 00 66 06 B2 01 00 "
    "01 45 00 06 01 04 00 64 05 40",
    "21 13 00 0E 00 64 00 66 00 1B 01 00 DC F9 21 00 86 84 00 64 00 00 00 66 06 B2 01 00 "
    "01 45 00 06 01 04 00 65 00 02",
    "21 13 00 0E 00 64 00 66 00 1C 01 00 DC F8 21 00 86 84 00 64 00 00 00 66 06 B2 01 00 "
    "01 45 00 06 01 04 00 66 00 02",
    "21 13 00 0E 00 64 00 66 00 1D 01 00 DC F7 21 00 86 84 00 64 00 00 00 66 06 B2 01 00 "
    "01 45 00 06 01 04 00 67 00 02",
    "21 13 00 0E 00 64 00 66 00 1E 01 00 DC F6 21 00 86 84 00 64 00 00 00 66 06 B2 01 00 "
    "01 45 00 06 01 04 00 C8 00 02",
    "21 13 00 0E 00 64 00 66 00 1F 01 00 DC F5 21 00 86 84 00 64 00 00 00 66 06 B2 01 00 "
    "01 45 00 06 01 04 4E 1F 00 02",
};

/* Replies 100 -> 102, frames #51, #99, ... #675; D100's count 0x0011 .. 0x001E. */
static const char *const CAPTURED_REP[WALK_STEPS] = {
    "21 13 00 0E 00 66 00 64 00 11 01 00 DD 03 21 00 86 60 00 66 06 B2 00 66 06 B2 01 00 "
    "01 00 00 0C 01 04 00 64 00 01 FE 04 44 31 30 30",
    "21 13 00 0E 00 66 00 64 00 12 01 00 DD 02 21 00 86 60 00 66 06 B2 00 66 06 B2 01 00 "
    "01 00 00 10 01 04 00 64 01 DA FE 08 2A 58 4D 2D 46 49 44 4F",
    "21 13 00 0E 00 66 00 64 00 13 01 00 DD 01 21 00 86 60 00 66 06 B2 00 66 06 B2 01 00 "
    "01 00 00 12 01 04 00 64 02 27 FE 06 2A 58 46 54 52 41 03 02 00 01",
    "21 13 00 0E 00 66 00 64 00 14 01 00 DD 00 21 00 86 60 00 66 06 B2 00 66 06 B2 01 00 "
    "01 00 00 12 01 04 00 64 02 D0 FE 09 2A 58 4D 2D 45 4E 4E 53 30 00",
    "21 13 00 0E 00 66 00 64 00 15 01 00 DC FF 21 00 86 60 00 66 06 B2 00 66 06 B2 01 00 "
    "01 00 00 0E 01 04 00 64 03 AC FE 06 2A 43 4F 53 50 4F",
    "21 13 00 0E 00 66 00 64 00 16 01 00 DC FE 21 00 86 60 00 66 06 B2 00 66 06 B2 01 00 "
    "01 00 00 10 01 04 00 64 04 01 FE 07 2A 54 41 44 41 44 4D 00",
    "21 13 00 0E 00 66 00 64 00 17 01 00 DC FD 21 00 86 60 00 66 06 B2 00 66 06 B2 01 00 "
    "01 00 00 14 01 04 00 64 04 CE FE 07 2A 46 41 2D 46 53 41 00 03 02 00 02",
    "21 13 00 0E 00 66 00 64 00 18 01 00 DC FC 21 00 86 60 00 66 06 B2 00 66 06 B2 01 00 "
    "01 00 00 16 01 04 00 64 05 3F FE 09 2A 46 41 2D 46 53 41 2D 49 00 03 02 00 00",
    "21 13 00 0E 00 66 00 64 00 19 01 00 DC FB 21 00 86 60 00 66 06 B2 00 66 06 B2 01 00 "
    "01 00 00 0C 01 04 00 65 00 01 FE 04 44 31 30 31",
    "21 13 00 0E 00 66 00 64 00 1A 01 00 DC FA 21 00 86 60 00 66 06 B2 00 66 06 B2 01 00 "
    "01 00 00 0C 01 04 00 66 00 01 FE 04 44 31 30 32",
    "21 13 00 0E 00 66 00 64 00 1B 01 00 DC F9 21 00 86 60 00 66 06 B2 00 66 06 B2 01 00 "
    "01 00 00 0C 01 04 00 67 00 01 FE 04 44 31 30 33",
    "21 13 00 0E 00 66 00 64 00 1C 01 00 DC F8 21 00 86 60 00 66 06 B2 00 66 06 B2 01 00 "
    "01 00 00 0C 01 04 00 C8 00 01 FE 04 44 32 30 30",
    "21 13 00 0E 00 66 00 64 00 1D 01 00 DC F7 21 00 86 60 00 66 06 B2 00 66 06 B2 01 00 "
    "01 00 00 0E 01 04 4E 1F 00 01 FE 06 44 31 39 39 39 39",
    "21 13 00 0E 00 66 00 64 00 1E 01 00 DC F6 21 00 86 60 00 66 06 B2 00 66 06 B2 01 00 "
    "01 00 00 04 01 02 00 00",
};

/* D100's table as the walk read it (system, port word, name, free seats). D100 itself is the
 * automatic own-name row and is not in this list. */
typedef struct TableRow
{
    uint16_t system;
    uint16_t port;
    const char *name;
    int32_t free;
} TableRow;

static const TableRow D100_ROWS[] = {
    {100, 0x01DA, "*XM-FIDO", XROUT_NAME_NO_FREE}, {100, 0x0227, "*XFTRA", 1},
    {100, 0x02D0, "*XM-ENNS0", XROUT_NAME_NO_FREE}, {100, 0x03AC, "*COSPO", XROUT_NAME_NO_FREE},
    {100, 0x0401, "*TADADM", XROUT_NAME_NO_FREE},  {100, 0x04CE, "*FA-FSA", 2},
    {100, 0x053F, "*FA-FSA-I", 0},                 {101, 0x0001, "D101", XROUT_NAME_NO_FREE},
    {102, 0x0001, "D102", XROUT_NAME_NO_FREE},     {103, 0x0001, "D103", XROUT_NAME_NO_FREE},
    {200, 0x0001, "D200", XROUT_NAME_NO_FREE},     {19999, 0x0001, "D19999", XROUT_NAME_NO_FREE},
};
#define D100_ROW_COUNT ((int)(sizeof D100_ROWS / sizeof D100_ROWS[0]))

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

static XmsgNode s_a; /* 100, answers */
static XmsgNode s_b; /* 102, asks */
static TestPair s_pair;
static TestPairPort s_port_a;
static TestPairPort s_port_b;
static Store s_store_a;
static Store s_store_b;

/* What B caught: datagrams no session claimed, and A's port-0 letters. */
#define CAUGHT_MAX 24
static uint8_t s_caught[CAUGHT_MAX][80];
static size_t s_caught_len[CAUGHT_MAX];
static int s_caught_count;
static int s_letters_a;

static void b_unhandled(void *ctx, const XmsgDatagram *dg)
{
    (void)ctx;
    if (s_caught_count < CAUGHT_MAX && dg->has_subhdr && dg->body_len + XMSG_DG_HDR_SIZE <= 80)
    {
        /* The body points into the received info field, 28 bytes after its start. */
        memcpy(s_caught[s_caught_count], dg->body - XMSG_DG_HDR_SIZE,
               dg->body_len + XMSG_DG_HDR_SIZE);
        s_caught_len[s_caught_count] = dg->body_len + XMSG_DG_HDR_SIZE;
        s_caught_count++;
    }
}

static void a_on_letter(void *ctx, const XmsgDatagram *dg, uint32_t now_ms)
{
    (void)ctx;
    (void)dg;
    (void)now_ms;
    s_letters_a++;
}

static void node_init(XmsgNode *n, uint16_t node, Store *store, XmsgTransportPort *tp,
                      bool answer_names, const char *system_name)
{
    XmsgNodeConfig cfg;
    XmsgPorts ports;
    XmsgNodeCallbacks cb;
    xmsg_node_config_default(&cfg, node);
    cfg.lapb.t1_ms = 50;
    cfg.lapb.t3_ms = 2000;
    cfg.settle_ms = 20;
    cfg.answer_names = answer_names;
    if (system_name != NULL)
    {
        (void)snprintf(cfg.system_name, sizeof cfg.system_name, "%s", system_name);
    }
    cfg.reject_closed_ports = (node != 102); /* 102 is a tap: it takes every datagram through
                                              * unhandled */
    memset(&ports, 0, sizeof ports);
    ports.transport = *tp;
    ports.flags1.ctx = store;
    ports.flags1.load = store_load;
    ports.flags1.save = store_save;
    memset(&cb, 0, sizeof cb);
    if (node == 102)
    {
        cb.unhandled = b_unhandled;
    }
    else
    {
        cb.on_letter = a_on_letter;
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

/* Bring the pair up. `a_answers`: A answers XSGNI. `a_name`: A's configured system name. */
static uint32_t bring_up(bool a_answers, const char *a_name)
{
    XmsgTransportPort ta;
    XmsgTransportPort tb;
    uint32_t now = 0;
    memset(&s_store_a, 0, sizeof s_store_a);
    memset(&s_store_b, 0, sizeof s_store_b);
    /* D100's next number to 102 when the capture starts: its first reply is 0x0011. */
    (void)store_save(&s_store_a, 102, 0x0011);
    s_caught_count = 0;
    s_letters_a = 0;
    test_pair_init(&s_pair);
    test_pair_transport(&s_pair, 0, &s_port_a, &ta);
    test_pair_transport(&s_pair, 1, &s_port_b, &tb);
    node_init(&s_a, 100, &s_store_a, &ta, a_answers, a_name);
    node_init(&s_b, 102, &s_store_b, &tb, true, NULL);
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

static void fill_d100_table(XmsgNode *n)
{
    XroutNameRow r;
    for (int i = 0; i < D100_ROW_COUNT; i++)
    {
        CHECK_EQ(xrout_name_row_set(&r, D100_ROWS[i].system, D100_ROWS[i].port, D100_ROWS[i].name,
                                    D100_ROWS[i].free),
                 XMSG_OK);
        CHECK_EQ(xmsg_node_name_put(n, &r), XMSG_OK);
    }
}

/* Put raw info bytes into B's window, as the asking machine would send them. */
static void asker_sends(const uint8_t *info, size_t len, uint32_t now)
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

static uint32_t send_captured(int i, uint32_t now)
{
    uint8_t raw[64];
    int n = test_hex_to_bytes(CAPTURED_REQ[i], raw, sizeof raw);
    CHECK(n > 0);
    asker_sends(raw, (size_t)n, now);
    return run(now, 300);
}

/* A request of the captured shape at Flags1 `f1` from 102 port 06B2, with role byte `role`. */
static size_t asker_request(uint8_t *out, size_t cap, uint16_t f1, uint8_t role, uint32_t asked)
{
    uint8_t body[XROUT_NAME_REQ_MAX];
    XmsgSubhdr sub = {0x86, role, 100, 0x0000, 102, 0x06B2, XROUT_NAME_XMCSM};
    size_t len = xrout_name_request_write(body, asked);
    return xmsg_dg_write_data(out, cap, 0x0E, 100, 102, f1, XROUT_NAME_XMCSM, &sub, body, len);
}

/* ------------------------------------------------------------------ the answer side */

static void node_answers_the_captured_walk(void)
{
    uint32_t now = bring_up(true, "D100");
    uint32_t acks_before = s_b.stats.acks_received;
    fill_d100_table(&s_a);

    for (int i = 0; i < WALK_STEPS; i++)
    {
        now = send_captured(i, now);
        CHECK_EQ(s_caught_count, i + 1);
        CHECK_BYTES(s_caught[i], s_caught_len[i], CAPTURED_REP[i]);
    }
    CHECK_EQ(s_a.stats.name_requests, WALK_STEPS);
    CHECK_EQ(s_a.stats.name_replies, WALK_STEPS);
    CHECK_EQ(s_a.stats.name_bad, 0);
    CHECK_EQ(s_a.stats.route_replies, 0);
    /* Every request was acknowledged, as in the capture, and each reply spent one number. */
    CHECK_EQ(s_b.stats.acks_received - acks_before, WALK_STEPS);
    CHECK_EQ(xmsg_flags1_peek(&s_a.flags1, 102), 0x001F);
    CHECK_EQ(s_a.stats.unhandled, 0); /* none of it went to the application */
    CHECK_EQ(s_letters_a, 0);
}

static void repeated_request_gets_the_same_reply(void)
{
    uint32_t now = bring_up(true, "D100");
    fill_d100_table(&s_a);

    now = send_captured(0, now);
    /* The same request again (same Flags1 0x0012): the same reply at the same number 0x0011. */
    now = send_captured(0, now);
    CHECK_EQ(s_a.stats.repeats, 1);
    CHECK_EQ(s_caught_count, 2);
    CHECK_BYTES(s_caught[1], s_caught_len[1], CAPTURED_REP[0]);
    CHECK_EQ(xmsg_flags1_peek(&s_a.flags1, 102), 0x0012);

    /* The walk goes on as captured: no number was spent on the repeat. */
    now = send_captured(1, now);
    CHECK_BYTES(s_caught[2], s_caught_len[2], CAPTURED_REP[1]);
    now = send_captured(1, now);
    CHECK_BYTES(s_caught[3], s_caught_len[3], CAPTURED_REP[1]);
    now = send_captured(2, now);
    (void)now;
    CHECK_EQ(s_caught_count, 5);
    CHECK_BYTES(s_caught[4], s_caught_len[4], CAPTURED_REP[2]);
    CHECK_EQ(s_a.stats.name_replies, 5);
}

static void role_c4_and_default_name(void)
{
    uint8_t req[64];
    XmsgDatagram dg;
    XroutNameRow r;
    bool is_end = true;
    uint8_t status = 0xEE;

    /* No configured name and no rows added: the table is "D100" alone. */
    uint32_t now = bring_up(true, NULL);
    /* D100's own request to us had role byte C4 instead of 84 (live 2026-11-10 10:33:23). */
    size_t rl = asker_request(req, sizeof req, 0x0012, 0xC4, 0);
    asker_sends(req, rl, now);
    now = run(now, 300);
    CHECK_EQ(s_caught_count, 1);
    CHECK_BYTES(s_caught[0], s_caught_len[0], CAPTURED_REP[0]);

    /* Past our own row: the end form. */
    rl = asker_request(req, sizeof req, 0x0013, 0xC4, 0x00640002u);
    asker_sends(req, rl, now);
    (void)run(now, 300);
    CHECK_EQ(s_caught_count, 2);
    CHECK_EQ(xmsg_dg_parse(s_caught[1], s_caught_len[1], &dg), XMSG_OK);
    CHECK_EQ(dg.hdr.flags1, 0x0012);
    CHECK_BYTES(dg.body, dg.body_len, "0100000401020000");
    CHECK_EQ(xrout_name_reply_read(dg.body, dg.body_len, &status, &r, &is_end), XMSG_OK);
    CHECK(is_end);

    /* The table the node answers from. */
    const XroutNameTable *t = xmsg_node_names(&s_a);
    CHECK(t != NULL);
    CHECK_EQ(t->count, 1);
    CHECK(strcmp(t->r[0].name, "D100") == 0);
}

static void answer_names_off_passes_the_letter_on(void)
{
    uint32_t now = bring_up(false, "D100");
    (void)send_captured(0, now);
    CHECK_EQ(s_caught_count, 0);
    CHECK_EQ(s_a.stats.name_requests, 0);
    CHECK_EQ(s_letters_a, 1);
}

/* ------------------------------------------------------------------ the walk */

typedef struct WalkResult
{
    XroutNameRow r[20];
    int count;
    bool done;
    int result;
    unsigned rows;
} WalkResult;

static void on_row(void *ctx, const XroutNameRow *row)
{
    WalkResult *r = (WalkResult *)ctx;
    if (r->count < 20)
    {
        r->r[r->count++] = *row;
    }
}

static void on_done(void *ctx, int result, unsigned rows)
{
    WalkResult *r = (WalkResult *)ctx;
    r->done = true;
    r->result = result;
    r->rows = rows;
}

static void walk_reads_the_whole_table(void)
{
    static XroutNamesWalk w;
    WalkResult r;
    uint32_t now = bring_up(true, "D100");
    memset(&r, 0, sizeof r);
    memset(&w, 0, sizeof w);
    fill_d100_table(&s_a);

    /* B (102) walks A (100), the link neighbour. */
    XroutNamesWalkCallbacks cb = {&r, on_row, on_done};
    uint16_t f1_before = xmsg_flags1_peek(&s_b.flags1, 100);
    CHECK_EQ(xrout_names_walk_start(&w, &s_b, 100, 100, 2000, &cb, now), XMSG_OK);
    CHECK(xrout_names_walk_is_busy(&w));
    CHECK_EQ(xrout_names_walk_start(&w, &s_b, 100, 100, 2000, &cb, now), XMSG_EAGAIN);
    for (int i = 0; i < 10000 && !r.done; i++)
    {
        now++;
        pump_pair(now);
    }
    CHECK(r.done);
    CHECK_EQ(r.result, XMSG_OK);
    CHECK_EQ(r.rows, D100_ROW_COUNT + 1);
    CHECK_EQ(r.count, D100_ROW_COUNT + 1);
    CHECK_EQ(r.r[0].system, 100);
    CHECK_EQ(r.r[0].port, 0x0001);
    CHECK(strcmp(r.r[0].name, "D100") == 0);
    for (int i = 0; i < D100_ROW_COUNT && i + 1 < r.count; i++)
    {
        const XroutNameRow *got = &r.r[i + 1];
        CHECK_EQ(got->system, D100_ROWS[i].system);
        CHECK_EQ(got->port, D100_ROWS[i].port);
        CHECK(strcmp(got->name, D100_ROWS[i].name) == 0);
        CHECK_EQ(got->free, D100_ROWS[i].free);
    }
    /* Fourteen questions (0, then each magic + 1, the last answered by the end form), each its
     * own number of ours. */
    CHECK_EQ(xmsg_flags1_peek(&s_b.flags1, 100), (uint16_t)(f1_before + WALK_STEPS));
    CHECK_EQ(s_a.stats.name_requests, WALK_STEPS);
    CHECK_EQ(s_a.stats.name_replies, WALK_STEPS);
    CHECK(!xrout_names_walk_is_busy(&w));
    CHECK(!w.base.attached); /* the session slot is free again */

    /* A second walk can start at once (on_done had freed the slot). */
    memset(&r, 0, sizeof r);
    CHECK_EQ(xrout_names_walk_start(&w, &s_b, 100, 100, 2000, &cb, now), XMSG_OK);
    for (int i = 0; i < 10000 && !r.done; i++)
    {
        now++;
        pump_pair(now);
    }
    CHECK_EQ(r.result, XMSG_OK);
    CHECK_EQ(r.rows, D100_ROW_COUNT + 1);

    /* Cancel says so through on_done. */
    memset(&r, 0, sizeof r);
    CHECK_EQ(xrout_names_walk_start(&w, &s_b, 100, 100, 2000, &cb, now), XMSG_OK);
    CHECK_EQ(xrout_names_walk_cancel(&w), XMSG_OK);
    CHECK(r.done);
    CHECK_EQ(r.result, XMSG_ESTATE);
    CHECK(!w.base.attached);
}

static void walk_times_out_when_the_remote_is_silent(void)
{
    static XroutNamesWalk w;
    WalkResult r;
    uint32_t now = bring_up(false, "D100"); /* A acknowledges but does not answer */
    memset(&r, 0, sizeof r);
    memset(&w, 0, sizeof w);
    XroutNamesWalkCallbacks cb = {&r, on_row, on_done};
    CHECK_EQ(xrout_names_walk_start(&w, &s_b, 100, 100, 500, &cb, now), XMSG_OK);
    uint32_t start = now;
    for (int i = 0; i < 3000 && !r.done; i++)
    {
        now++;
        pump_pair(now);
    }
    CHECK(r.done);
    CHECK_EQ(r.result, XMSG_ETIMEDOUT);
    CHECK_EQ(r.rows, 0);
    CHECK(now - start >= 500);
    CHECK(now - start < 600);
    CHECK_EQ(s_letters_a, 1); /* the question reached A and went to its application */
    CHECK(!w.base.attached);
}

static void walk_needs_a_ready_link(void)
{
    static XroutNamesWalk w;
    static XmsgNode lone;
    XmsgNodeConfig cfg;
    XmsgPorts ports;
    XmsgTransportPort tp;
    WalkResult r;
    TestPair p;
    TestPairPort pp;
    test_pair_init(&p);
    test_pair_transport(&p, 0, &pp, &tp);
    xmsg_node_config_default(&cfg, 102);
    memset(&ports, 0, sizeof ports);
    ports.transport = tp;
    CHECK_EQ(xmsg_node_init(&lone, &cfg, &ports, NULL), XMSG_OK);
    memset(&w, 0, sizeof w);
    XroutNamesWalkCallbacks cb = {&r, on_row, on_done};
    CHECK_EQ(xrout_names_walk_start(&w, &lone, 100, 100, 500, &cb, 0), XMSG_ESTATE);
    CHECK_EQ(xrout_names_walk_start(NULL, &lone, 100, 100, 500, &cb, 0), XMSG_EINVAL);
    CHECK_EQ(xrout_names_walk_cancel(&w), XMSG_ESTATE);
    CHECK(!xrout_names_walk_is_busy(&w));
}

int main(void)
{
    printf("[pair_names]\n");
    node_answers_the_captured_walk();
    repeated_request_gets_the_same_reply();
    role_c4_and_default_name();
    answer_names_off_passes_the_letter_on();
    walk_reads_the_whole_table();
    walk_times_out_when_the_remote_is_silent();
    walk_needs_a_ready_link();
    TEST_DONE();
}
