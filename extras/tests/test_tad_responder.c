/*
 * test_tad_responder.c - the answer side of CONNECT-TO, replayed against a real SINTRAN host.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * The replay runs two real nodes on the in-memory pair. Node 102 carries our responder, its
 * count to 100 starting at the captured 0x012F. Node 100 plays the real D100 asker: it sends the
 * CAPTURED asker frames at their captured Flags1, each one once the responder has sent every
 * host frame the real D102 sent before it. The test application on 102 types the real D102's
 * text through the API. Every frame the responder sends must equal the real D102's frame byte
 * for byte, header checksum included; only our two port words differ (the node mints them), and
 * the test patches them into the expected frames.
 *
 * Then our own asker talks to the responder (peer to peer), and the edges: refusal, status
 * question, escape on and off, the asker hanging up, the output window, BDAT splitting.
 */
#include "libxmsg/console/console.h"
#include "libxmsg/tad/tad_asker.h"
#include "libxmsg/tad/tad_codec.h"
#include "libxmsg/tad/tad_responder.h"
#include "libxmsg/xmsg/xmsg_node.h"
#include "libxmsg/xmsg_endian.h"
#include "libxmsg/xrout/xrout_letter.h"

#include "pair/test_pair.h"
#include "test_util.h"
#include "vectors/tad_connect_d100_d102.h"

#define N_ASKER (sizeof k_tad_asker_frames / sizeof k_tad_asker_frames[0])
#define N_HOST  (sizeof k_tad_host_frames / sizeof k_tad_host_frames[0])

#define OFF_DST_PORT 20
#define OFF_SRC_PORT 24

/* ------------------------------------------------------------------ chains */

static void host_chains_are_laid_out_like_the_host(void)
{
    uint8_t buf[64];
    TadWriter w;
    static const uint8_t bmmx[3] = {0x01, 0x00, 0x00};
    static const uint8_t on = 0x01;
    static const uint8_t off = 0xFF;

    /* Banner head (record 0135): 00 04 03 01 00 00 00 03 01 01 01 .. */
    tad_writer_init(&w, buf, sizeof buf);
    CHECK_EQ(tad_writer_add_host(&w, TAD_OP_BMMX, bmmx, 3), XMSG_OK);
    CHECK_EQ(tad_writer_add_host(&w, TAD_OP_ECKM, &on, 1), XMSG_OK);
    CHECK_EQ(tad_writer_add_host(&w, TAD_OP_BDAT, (const uint8_t *)"\r\n", 2), XMSG_OK);
    CHECK_BYTES(buf, tad_writer_length(&w), "0004030100000003010101020D0A");
    /* Password prompt (record 0137): 01 0A <10> 00 03 01 FF 02 00 */
    tad_writer_init(&w, buf, sizeof buf);
    (void)tad_writer_add_host(&w, TAD_OP_BDAT, (const uint8_t *)"PASSWORD: ", 10);
    (void)tad_writer_add_host(&w, TAD_OP_ECKM, &off, 1);
    (void)tad_writer_add_host(&w, TAD_OP_RFI, NULL, 0);
    CHECK_BYTES(buf, tad_writer_length(&w), "010A50415353574F52443A20000301FF0200");
    /* An odd end before a normal message gets one fill (record 0142: 01 01 5E 00 02 00). */
    tad_writer_init(&w, buf, sizeof buf);
    (void)tad_writer_add_host(&w, TAD_OP_BDAT, (const uint8_t *)"^", 1);
    (void)tad_writer_add_host(&w, TAD_OP_RFI, NULL, 0);
    CHECK_BYTES(buf, tad_writer_length(&w), "01015E000200");
    /* Too small: nothing half-written is reported. */
    tad_writer_init(&w, buf, 3);
    CHECK_EQ(tad_writer_add_host(&w, TAD_OP_ECKM, &on, 1), XMSG_ENOSPC);
    CHECK_EQ(tad_writer_length(&w), 0);
}

/* ------------------------------------------------------------------ two nodes */

typedef struct Store
{
    uint16_t next;
    uint16_t peer;
} Store;

static int store_load(void *ctx, uint16_t node, uint16_t *next)
{
    Store *st = (Store *)ctx;
    if (node != st->peer)
    {
        return -1;
    }
    *next = st->next;
    return 0;
}

static int store_save(void *ctx, uint16_t node, uint16_t next)
{
    Store *st = (Store *)ctx;
    if (node == st->peer)
    {
        st->next = next;
    }
    return 0;
}

#define MAX_SEEN 64

typedef struct Seen
{
    uint8_t frames[MAX_SEEN][640];
    size_t lens[MAX_SEEN];
    uint16_t flags1[MAX_SEEN];
    size_t count;
} Seen;

static XmsgNode s_a; /* node 100: the asker's machine */
static XmsgNode s_b; /* node 102: ours, with the responder */
static TestPair s_pair;
static TestPairPort s_port_a;
static TestPairPort s_port_b;
static Store s_store_a;
static Store s_store_b;
static Seen s_seen; /* frames node 100 received for no session: the responder's */
static TadResponder s_resp;
static TadAsker s_asker;
static bool s_use_responder = true; /* node 102's on_letter offers letters to s_resp */

static void keep(void *ctx, const XmsgDatagram *dg)
{
    Seen *h = (Seen *)ctx;
    if (h->count >= MAX_SEEN)
    {
        CHECK(false);
        return;
    }
    h->lens[h->count] = xmsg_dg_write_data(
        h->frames[h->count], sizeof h->frames[0], dg->hdr.subtype, dg->hdr.dst_node,
        dg->hdr.src_node, dg->hdr.flags1, dg->hdr.flags2, &dg->sub, dg->body, dg->body_len);
    h->flags1[h->count] = dg->hdr.flags1;
    h->count++;
}

static int s_offer_rc;

static void b_on_letter(void *ctx, const XmsgDatagram *dg, uint32_t now_ms)
{
    (void)ctx;
    if (s_use_responder)
    {
        s_offer_rc = tad_responder_offer(&s_resp, &s_b, dg, now_ms);
    }
}

static void pump(uint32_t now)
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

static uint32_t run(uint32_t now, int ms)
{
    for (int i = 0; i < ms; i++)
    {
        now++;
        pump(now);
    }
    return now;
}

static uint32_t bring_up(uint16_t a_first, uint16_t b_first)
{
    XmsgTransportPort ta;
    XmsgTransportPort tb;
    XmsgNodeConfig cfg;
    XmsgPorts ports;
    XmsgNodeCallbacks cb;
    uint32_t now = 0;

    memset(&s_seen, 0, sizeof s_seen);
    s_store_a.peer = 102;
    s_store_a.next = a_first;
    s_store_b.peer = 100;
    s_store_b.next = b_first;
    test_pair_init(&s_pair);
    test_pair_transport(&s_pair, 0, &s_port_a, &ta);
    test_pair_transport(&s_pair, 1, &s_port_b, &tb);

    xmsg_node_config_default(&cfg, 100);
    cfg.lapb.t1_ms = 50;
    cfg.settle_ms = 20;
    cfg.reject_closed_ports = false; /* a tap: it takes every datagram through unhandled */
    memset(&ports, 0, sizeof ports);
    ports.transport = ta;
    ports.flags1.ctx = &s_store_a;
    ports.flags1.load = store_load;
    ports.flags1.save = store_save;
    memset(&cb, 0, sizeof cb);
    cb.ctx = &s_seen;
    cb.unhandled = keep;
    CHECK_EQ(xmsg_node_init(&s_a, &cfg, &ports, &cb), XMSG_OK);

    xmsg_node_config_default(&cfg, 102);
    cfg.lapb.t1_ms = 50;
    cfg.settle_ms = 20;
    memset(&ports, 0, sizeof ports);
    ports.transport = tb;
    ports.flags1.ctx = &s_store_b;
    ports.flags1.load = store_load;
    ports.flags1.save = store_save;
    memset(&cb, 0, sizeof cb);
    cb.on_letter = b_on_letter;
    CHECK_EQ(xmsg_node_init(&s_b, &cfg, &ports, &cb), XMSG_OK);

    xmsg_node_link_start(&s_a, now);
    xmsg_node_link_start(&s_b, now);
    while (!(xmsg_node_is_ready(&s_a) && xmsg_node_is_ready(&s_b)) && now < 5000)
    {
        now++;
        pump(now);
    }
    CHECK(xmsg_node_is_ready(&s_a) && xmsg_node_is_ready(&s_b));
    return now;
}

/* ------------------------------------------------------------------ the scripted host */

/* What the test application on 102 saw and does. */
typedef struct App
{
    TadResponderState states[16];
    int reasons[16];
    size_t state_count;
    uint8_t typed[1024];
    size_t typed_len;
    unsigned inputs;
    unsigned escapes;
    TadResponderCaller who;
    unsigned calls;
    int verdict;
    bool replay;            /* script the captured D102 text */
    const uint8_t *pending; /* text still to write (the listing) */
    size_t pending_len;
    int after_pending; /* what follows it: 1 = prompt after the listing */
    uint8_t listing[2100];
    size_t listing_len;
    uint8_t banner[128];
    size_t banner_len;
    uint8_t clock[64];
    size_t clock_len;
} App;

static App s_app;

static int app_incoming(void *ctx, const TadResponderCaller *who)
{
    App *a = (App *)ctx;
    a->who = *who;
    a->calls++;
    return a->verdict;
}

static void wr(const char *t)
{
    CHECK_EQ(tad_responder_write(&s_resp, (const uint8_t *)t, strlen(t)), (int)strlen(t));
}

static void prompt(void)
{
    CHECK_EQ(tad_responder_login_state(&s_resp, TAD_SYCN_LOGGED_IN), XMSG_OK);
    wr("R@");
    CHECK_EQ(tad_responder_grant_input(&s_resp), XMSG_OK);
    CHECK_EQ(tad_responder_send(&s_resp), XMSG_OK);
}

static void app_state(void *ctx, TadResponderState st, int reason)
{
    App *a = (App *)ctx;
    if (a->state_count < 16)
    {
        a->states[a->state_count] = st;
        a->reasons[a->state_count] = reason;
        a->state_count++;
    }
    if (st == TAD_RESP_RUNNING && a->replay)
    {
        /* Record 0135: BMMX 01 0000, ECKM 01, the banner, SYCN 0002, "ENTER ", RFI. */
        static const uint8_t bmmx[3] = {0x01, 0x00, 0x00};
        CHECK_EQ(tad_responder_put(&s_resp, TAD_OP_BMMX, bmmx, 3), XMSG_OK);
        CHECK_EQ(tad_responder_echo(&s_resp, 0x01), XMSG_OK);
        CHECK_EQ(tad_responder_write(&s_resp, a->banner, a->banner_len), (int)a->banner_len);
        CHECK_EQ(tad_responder_login_state(&s_resp, TAD_SYCN_WAIT_USER), XMSG_OK);
        wr("\r\nENTER ");
        CHECK_EQ(tad_responder_grant_input(&s_resp), XMSG_OK);
        CHECK_EQ(tad_responder_send(&s_resp), XMSG_OK);
    }
}

static void app_escape(void *ctx)
{
    ((App *)ctx)->escapes++;
}

static bool s_status_cb;

static void app_status(void *ctx, uint16_t *in_use, uint16_t *total)
{
    (void)ctx;
    if (s_status_cb)
    {
        *in_use = 3;
        *total = 8;
    }
}

static void app_text_in(void *ctx, const uint8_t *bytes, size_t len)
{
    App *a = (App *)ctx;
    if (a->typed_len + len <= sizeof a->typed)
    {
        memcpy(a->typed + a->typed_len, bytes, len);
        a->typed_len += len;
    }
    a->inputs++;
    CHECK(!tad_responder_input_granted(&s_resp)); /* the BDAT used the grant */
    if (!a->replay)
    {
        return;
    }
    switch (a->inputs)
    {
    case 1: /* "sys" CR: records 0136 and 0137 */
        wr("\r\n");
        CHECK_EQ(tad_responder_login_state(&s_resp, TAD_SYCN_USER_TAKEN), XMSG_OK);
        CHECK_EQ(tad_responder_set_escape(&s_resp, false), XMSG_OK);
        CHECK_EQ(tad_responder_send(&s_resp), XMSG_OK);
        wr("PASSWORD: ");
        CHECK_EQ(tad_responder_echo(&s_resp, 0xFF), XMSG_OK);
        CHECK_EQ(tad_responder_grant_input(&s_resp), XMSG_OK);
        CHECK_EQ(tad_responder_send(&s_resp), XMSG_OK);
        break;
    case 2: /* CR: records 0138 and 0139 */
        wr("\r\n");
        CHECK_EQ(tad_responder_echo(&s_resp, 0x01), XMSG_OK);
        wr("OK");
        CHECK_EQ(tad_responder_login_state(&s_resp, TAD_SYCN_PASSWORD_OK), XMSG_OK);
        CHECK_EQ(tad_responder_set_escape(&s_resp, true), XMSG_OK);
        CHECK_EQ(tad_responder_send(&s_resp), XMSG_OK);
        wr("\r\n");
        prompt();
        break;
    case 3: /* the LIST-FILES command: records 013A-0141, written as the room allows */
        a->pending = a->listing;
        a->pending_len = a->listing_len;
        a->after_pending = 1;
        break;
    case 4: /* record 0142 */
        wr("^");
        CHECK_EQ(tad_responder_grant_input(&s_resp), XMSG_OK);
        CHECK_EQ(tad_responder_send(&s_resp), XMSG_OK);
        break;
    case 5: /* LOGOUT: records 0143-0146, then the hang-up (0147 and our DCON) */
    {
        static const uint8_t bmmx0[3] = {0x00, 0x00, 0x00};
        CHECK_EQ(tad_responder_write(&s_resp, a->clock, a->clock_len), (int)a->clock_len);
        CHECK_EQ(tad_responder_set_escape(&s_resp, false), XMSG_OK);
        CHECK_EQ(tad_responder_send(&s_resp), XMSG_OK);
        CHECK_EQ(tad_responder_put(&s_resp, TAD_OP_BMMX, bmmx0, 3), XMSG_OK);
        CHECK_EQ(tad_responder_echo(&s_resp, 0x00), XMSG_OK);
        CHECK_EQ(tad_responder_set_escape(&s_resp, false), XMSG_OK);
        CHECK_EQ(tad_responder_send(&s_resp), XMSG_OK);
        wr("\r\n--EXIT--\r\n");
        CHECK_EQ(tad_responder_login_state(&s_resp, TAD_SYCN_LOGOUT), XMSG_OK);
        CHECK_EQ(tad_responder_send(&s_resp), XMSG_OK);
        CHECK_EQ(tad_responder_set_escape(&s_resp, true), XMSG_OK);
        CHECK_EQ(tad_responder_send(&s_resp), XMSG_OK);
        break;
    }
    default:
        break;
    }
}

/* Write what the chains have room for; then what follows. */
static void app_feed(void)
{
    while (s_app.pending_len > 0)
    {
        int n = tad_responder_write(&s_resp, s_app.pending, s_app.pending_len);
        if (n <= 0)
        {
            return;
        }
        s_app.pending += n;
        s_app.pending_len -= (size_t)n;
    }
    if (s_app.after_pending == 1)
    {
        s_app.after_pending = 0;
        prompt();
    }
}

static void responder_start(bool replay)
{
    TadResponderConfig cfg;
    TadResponderCallbacks cb = {&s_app,      app_incoming, app_state,
                                app_text_in, app_escape,   app_status};
    memset(&s_app, 0, sizeof s_app);
    s_app.replay = replay;
    tad_responder_config_default(&cfg);
    cfg.logical_unit = 0x0304; /* what D102 handed out in this session */
    CHECK_EQ(tad_responder_init(&s_resp, &cfg, &cb), XMSG_OK);
}

/* The data of BDAT number `which` (0 = first) of a captured host frame. */
static size_t host_bdat(size_t frame, int which, uint8_t *out, size_t cap)
{
    uint8_t raw[640];
    int len = test_hex_to_bytes(k_tad_host_frames[frame].hex, raw, sizeof raw);
    TadReader r;
    TadMessage m;
    int k = 0;
    tad_reader_init(&r, raw + 32, (size_t)len - 32);
    while (tad_reader_next(&r, &m))
    {
        if (m.opcode == TAD_OP_BDAT)
        {
            if (k++ == which && m.len <= cap)
            {
                memcpy(out, m.data, m.len);
                return m.len;
            }
        }
    }
    return 0;
}

/* Send one captured asker frame from node 100 at its captured number, ports aimed at ours. */
static void asker_send_hex(const char *hex, uint16_t flags1, uint32_t now)
{
    uint8_t raw[640];
    XmsgDatagram dg;
    uint8_t *body;
    size_t cap;
    int len = test_hex_to_bytes(hex, raw, sizeof raw);
    uint16_t dst = xmsg_be16_read(raw + OFF_DST_PORT);
    CHECK(len > 28);
    if (dst == 0x0156)
    {
        xmsg_be16_write(raw + OFF_DST_PORT, s_resp.adm.base.local_port);
    }
    else if (dst == 0x04C2)
    {
        xmsg_be16_write(raw + OFF_DST_PORT, s_resp.term.base.local_port);
    }
    CHECK_EQ(xmsg_dg_parse(raw, (size_t)len, &dg), XMSG_OK);
    CHECK_EQ(xmsg_node_send_begin(&s_a, &body, &cap), XMSG_OK);
    memcpy(body, dg.body, dg.body_len);
    CHECK_EQ(xmsg_node_send_repeat(&s_a, 102, &dg.sub, dg.body_len, flags1, now), XMSG_OK);
}

static void replay_against_the_real_asker(void)
{
    uint32_t now = bring_up(0x00F8, 0x012F);
    size_t asker_next = 0;
    size_t checked = 0;
    uint8_t want[640];
    uint8_t piece[256];

    responder_start(true);
    /* The text the real D102 typed, taken from its frames. */
    s_app.banner_len = host_bdat(6, 0, s_app.banner, sizeof s_app.banner);
    s_app.clock_len = host_bdat(20, 0, s_app.clock, sizeof s_app.clock);
    for (size_t f = 11; f <= 18; f++)
    {
        size_t n = host_bdat(f, 0, piece, sizeof piece);
        memcpy(s_app.listing + s_app.listing_len, piece, n);
        s_app.listing_len += n;
    }
    CHECK_EQ(s_app.banner_len, 0x60);
    CHECK_EQ(s_app.listing_len, 7u * 255u + 247u);

    uint32_t start = now;
    while (now - start < 20000 && s_resp.state != TAD_RESP_CLOSED &&
           s_resp.state != TAD_RESP_FAILED)
    {
        now++;
        pump(now);
        app_feed();
        if (s_app.inputs == 5 && s_resp.state == TAD_RESP_RUNNING && checked >= N_HOST - 1)
        {
            CHECK_EQ(tad_responder_hangup(&s_resp, now), XMSG_OK); /* record 0147 follows */
        }

        /* Compare every new responder frame with the real D102's. */
        while (checked < s_seen.count && checked < N_HOST)
        {
            int wl = test_hex_to_bytes(k_tad_host_frames[checked].hex, want, sizeof want);
            uint16_t src = xmsg_be16_read(want + OFF_SRC_PORT);
            xmsg_be16_write(want + OFF_SRC_PORT, (src == 0x0156) ? s_resp.adm.base.local_port
                                                                 : s_resp.term.base.local_port);
            if (k_tad_host_frames[checked].flags1 == 0x0130)
            {
                xmsg_be16_write(want + 38, s_resp.term.base.local_port); /* in the 07 data */
            }
            CHECK_EQ(s_seen.lens[checked], (size_t)wl);
            if (s_seen.lens[checked] != (size_t)wl ||
                memcmp(s_seen.frames[checked], want, (size_t)wl) != 0)
            {
                printf("  responder frame %u (Flags1 0x%04X) differs from the capture\n",
                       (unsigned)checked, k_tad_host_frames[checked].flags1);
                CHECK(false);
            }
            checked++;
        }

        /* The next asker frame, once every host frame the real D102 sent before it is out. */
        if (asker_next < N_ASKER)
        {
            uint16_t f1 = k_tad_asker_frames[asker_next].flags1;
            bool ready = true;
            for (size_t j = 0; j < N_HOST; j++)
            {
                if (k_tad_host_frames[j].after < f1 && j >= s_seen.count)
                {
                    ready = false;
                }
            }
            if (ready && xmsg_node_is_ready(&s_a) && lapb_tx_free_slots(&s_a.lapb) > 0)
            {
                asker_send_hex(k_tad_asker_frames[asker_next].hex, f1, now);
                asker_next++;
                if (asker_next == 1)
                {
                    /* Once, the letter again at its old number: a repeat changes nothing. */
                    now = run(now, 5);
                    asker_send_hex(k_tad_asker_frames[0].hex, f1, now);
                }
            }
        }
    }

    CHECK_EQ(asker_next, N_ASKER);
    CHECK_EQ(checked, N_HOST);
    CHECK_EQ(s_resp.state, TAD_RESP_CLOSED);
    CHECK(!s_resp.adm.base.attached && !s_resp.term.base.attached);
    /* After the captured 0xFD: our DCON from the session port, the next number. */
    CHECK_EQ(s_seen.count, N_HOST + 1);
    if (s_seen.count == N_HOST + 1)
    {
        const uint8_t *f = s_seen.frames[N_HOST];
        CHECK_EQ(s_seen.flags1[N_HOST], 0x0148);
        CHECK_BYTES(f + 14, 4, "21008200");
        CHECK_EQ(xmsg_be16_read(f + OFF_SRC_PORT), s_resp.term.base.local_port);
        CHECK_BYTES(f + 26, s_seen.lens[N_HOST] - 26, "0008000000020900");
    }
    /* Who called, as the letter says. */
    CHECK_EQ(s_app.calls, 1);
    CHECK_EQ(s_app.who.peer_node, 100);
    CHECK_EQ(s_app.who.system, 100);
    CHECK_EQ(s_app.who.port, 0x02AB);
    CHECK(strcmp(s_app.who.service, "*TADADM") == 0);
    CHECK(strcmp(s_app.who.target, "D102") == 0);
    CHECK(s_resp.terminal.seen);
    CHECK_EQ(s_resp.terminal.tmod, 0x08);
    CHECK_EQ(s_resp.terminal.desc, 0x1B);
    CHECK_EQ(s_resp.terminal.opsv[2], 0x04);
    /* What the user typed, parity cleared: "sys" CR, CR, "li-fi,,," CR, "lou" ^A, "g" CR. */
    CHECK_EQ(s_app.inputs, 5);
    CHECK_BYTES(s_app.typed, s_app.typed_len, "7379730D0D6C692D66692C2C2C0D6C6F7501670D");
    CHECK(s_resp.stats.repeats_in >= 1);
    static const TadResponderState seq[] = {TAD_RESP_ACCEPTING, TAD_RESP_BRINGUP, TAD_RESP_RUNNING,
                                            TAD_RESP_CLOSING, TAD_RESP_CLOSED};
    CHECK_EQ(s_app.state_count, sizeof seq / sizeof seq[0]);
    for (size_t i = 0; i < s_app.state_count && i < sizeof seq / sizeof seq[0]; i++)
    {
        CHECK_EQ(s_app.states[i], seq[i]);
    }
    CHECK_EQ(s_app.reasons[s_app.state_count - 1], TAD_RESP_END_LOCAL);
    CHECK_EQ(s_a.stats.bad_headers + s_b.stats.bad_headers, 0);
    printf("  replay: %u responder frames equal the real D102's, %u asker frames replayed, "
           "%u ms\n",
           (unsigned)checked, (unsigned)asker_next, (unsigned)(now - start));
}

/* ------------------------------------------------------------------ peer to peer */

typedef struct Far
{
    uint8_t text[2048];
    size_t text_len;
    uint8_t eckm[8];
    size_t eckm_count;
    unsigned notices;
    TadAskerState last;
    int last_error;
} Far;

static Far s_far;

static void f_text(void *ctx, const uint8_t *b, size_t n)
{
    Far *f = (Far *)ctx;
    if (f->text_len + n <= sizeof f->text)
    {
        memcpy(f->text + f->text_len, b, n);
        f->text_len += n;
    }
}

static void f_state(void *ctx, TadAskerState st, int error)
{
    ((Far *)ctx)->last = st;
    ((Far *)ctx)->last_error = error;
}

static void f_echo(void *ctx, uint8_t e)
{
    Far *f = (Far *)ctx;
    if (f->eckm_count < sizeof f->eckm)
    {
        f->eckm[f->eckm_count++] = e;
    }
}

static void f_notice(void *ctx)
{
    ((Far *)ctx)->notices++;
}

static uint32_t peer_session(uint32_t now)
{
    TadAskerConfig cfg;
    TadAskerCallbacks cb = {&s_far, f_text, f_state, NULL, f_echo, f_notice, NULL};
    memset(&s_far, 0, sizeof s_far);
    tad_asker_config_default(&cfg, 102, "D102");
    CHECK_EQ(tad_asker_init(&s_asker, &cfg, &cb), XMSG_OK);
    CHECK_EQ(tad_asker_connect(&s_asker, &s_a, now), XMSG_OK);
    for (int i = 0; i < 3000 && s_resp.state != TAD_RESP_RUNNING; i++)
    {
        now = run(now, 1);
    }
    CHECK_EQ(s_resp.state, TAD_RESP_RUNNING);
    return now;
}

static void our_asker_talks_to_our_responder(void)
{
    uint32_t now = bring_up(0, 0);
    uint8_t big[600];

    responder_start(false);
    now = peer_session(now);
    CHECK_EQ(tad_asker_state(&s_asker), TAD_ASKER_TERMINAL_SETUP);

    /* A banner and a login prompt; the asker may type after the RFI. */
    CHECK_EQ(tad_responder_echo(&s_resp, 0x01), XMSG_OK);
    wr("\r\nWELCOME\r\nlogin: ");
    CHECK_EQ(tad_responder_grant_input(&s_resp), XMSG_OK);
    now = run(now, 300);
    CHECK_EQ(tad_asker_state(&s_asker), TAD_ASKER_RUNNING);
    CHECK(tad_asker_may_type(&s_asker));
    CHECK(s_far.text_len == 18 && memcmp(s_far.text, "\r\nWELCOME\r\nlogin: ", 18) == 0);
    CHECK(s_far.eckm_count == 1 && s_far.eckm[0] == 0x01);

    /* The user types; even parity on the wire, cleared for the application. Bit 7 data with
     * strip_parity off comes through untouched. */
    CHECK_EQ(tad_asker_write(&s_asker, (const uint8_t *)"SYSTEM\r", 7, now), 7);
    now = run(now, 300);
    CHECK(s_app.typed_len == 7 && memcmp(s_app.typed, "SYSTEM\r", 7) == 0);
    CHECK(!tad_asker_may_type(&s_asker));

    /* Long output: 600 bytes go as 255 + 255 + 90, each its own BDAT, in order. */
    for (size_t i = 0; i < sizeof big; i++)
    {
        big[i] = (uint8_t)(i & 0xFF); /* 8-bit clean: every byte value */
    }
    s_far.text_len = 0;
    CHECK_EQ(tad_responder_write(&s_resp, big, sizeof big), (int)sizeof big);
    CHECK_EQ(s_resp.c_closed, 2); /* two full BDATs closed their frames */
    CHECK_EQ(tad_responder_grant_input(&s_resp), XMSG_OK);
    now = run(now, 600);
    CHECK_EQ(s_far.text_len, sizeof big);
    CHECK(memcmp(s_far.text, big, sizeof big) == 0);
    CHECK(tad_asker_may_type(&s_asker));
    CHECK_EQ(s_resp.answers_owed, 0); /* the asker answered both content frames with DUMM */

    /* Escape: enabled -> the application hears it (ESRS back); disabled -> EDRS, not told. */
    CHECK_EQ(tad_asker_escape(&s_asker, now), XMSG_OK);
    now = run(now, 300);
    CHECK_EQ(s_app.escapes, 1);
    CHECK_EQ(tad_responder_set_escape(&s_resp, false), XMSG_OK);
    CHECK_EQ(tad_responder_send(&s_resp), XMSG_OK);
    now = run(now, 300);
    CHECK_EQ(tad_asker_escape(&s_asker, now), XMSG_OK);
    now = run(now, 300);
    CHECK_EQ(s_app.escapes, 1);
    CHECK_EQ(s_resp.stats.escapes_off, 1);

    /* Password: echo off goes through; then we hang up. The asker sees the notice and our
     * DCON and closes; so do we. */
    CHECK_EQ(tad_responder_echo(&s_resp, 0xFF), XMSG_OK);
    wr("\r\n--EXIT--\r\n");
    CHECK_EQ(tad_responder_login_state(&s_resp, TAD_SYCN_LOGOUT), XMSG_OK);
    CHECK_EQ(tad_responder_hangup(&s_resp, now), XMSG_OK);
    now = run(now, 1000);
    CHECK_EQ(s_resp.state, TAD_RESP_CLOSED);
    CHECK_EQ(tad_asker_state(&s_asker), TAD_ASKER_CLOSED);
    CHECK_EQ(s_asker.stats.host_hangups, 1);
    CHECK_EQ(s_asker.stats.logouts, 1);
    CHECK_EQ(s_far.notices, 1);
    CHECK(s_far.eckm_count >= 2 && s_far.eckm[s_far.eckm_count - 1] == 0xFF);
    CHECK_EQ(s_a.stats.bad_headers + s_b.stats.bad_headers, 0);
    CHECK_EQ(s_b.stats.unhandled, 0);

    /* The same responder takes the next call. */
    now = peer_session(now);
    CHECK_EQ(s_app.calls, 2); /* the second call to the same responder */
    /* The far user ends it: their DCON closes ours (reason remote). */
    CHECK_EQ(tad_responder_grant_input(&s_resp), XMSG_OK);
    now = run(now, 300);
    CHECK_EQ(tad_asker_hangup(&s_asker, now), XMSG_OK);
    now = run(now, 500);
    CHECK_EQ(s_resp.state, TAD_RESP_CLOSED);
    CHECK_EQ(s_app.reasons[s_app.state_count - 1], TAD_RESP_END_REMOTE);
    CHECK_EQ(tad_asker_state(&s_asker), TAD_ASKER_CLOSED);
    CHECK(!s_resp.adm.base.attached && !s_resp.term.base.attached);
    (void)now;
}

static void output_waits_for_the_askers_answers(void)
{
    uint32_t now = bring_up(0, 0);
    responder_start(false);
    now = peer_session(now);
    /* Three content frames without RFI: the third waits until the asker answers one. Here the
     * asker answers (our tad_asker, DUMM per frame), so all three arrive; the counter shows the
     * wait happened at most at the window. */
    wr("one");
    CHECK_EQ(tad_responder_send(&s_resp), XMSG_OK);
    wr("two");
    CHECK_EQ(tad_responder_send(&s_resp), XMSG_OK);
    wr("three");
    CHECK_EQ(tad_responder_send(&s_resp), XMSG_OK);
    /* Stop the asker side from reading: nothing reaches node 100, so no DUMM comes back. */
    for (int i = 0; i < 200; i++)
    {
        now++;
        xmsg_node_tick(&s_b, now);
        uint8_t chunk[256];
        size_t n;
        while ((n = test_pair_read(&s_pair, 1, chunk, sizeof chunk)) > 0)
        {
            xmsg_node_feed(&s_b, chunk, n, now);
        }
        CHECK(s_resp.answers_owed <= TAD_RESP_OUTPUT_WINDOW);
    }
    CHECK(s_resp.c_closed >= 1); /* "three" (at least) still waits */
    now = run(now, 600);
    CHECK_EQ(s_resp.c_closed, 0);
    CHECK(s_far.text_len == 11 && memcmp(s_far.text, "onetwothree", 11) == 0);
    tad_responder_abort(&s_resp);
    CHECK_EQ(s_resp.state, TAD_RESP_CLOSED);
    tad_asker_abort(&s_asker);
}

static void refusal(void)
{
    TadAskerConfig cfg;
    TadAskerCallbacks cb = {&s_far, f_text, f_state, NULL, f_echo, f_notice, NULL};
    uint32_t now = bring_up(0, 0);

    responder_start(false);
    s_app.verdict = 4;
    memset(&s_far, 0, sizeof s_far);
    tad_asker_config_default(&cfg, 102, "D102");
    CHECK_EQ(tad_asker_init(&s_asker, &cfg, &cb), XMSG_OK);
    CHECK_EQ(tad_asker_connect(&s_asker, &s_a, now), XMSG_OK);
    now = run(now, 500);
    CHECK_EQ(s_app.calls, 1);
    CHECK_EQ(s_resp.stats.refused, 1);
    CHECK_EQ(s_resp.state, TAD_RESP_IDLE);
    CHECK_EQ(tad_asker_state(&s_asker), TAD_ASKER_FAILED);
    CHECK_EQ(s_far.last_error, 4);
    CHECK(!s_resp.adm.base.attached);
}

/* Live D100, 2026-11-10: LI-SYS inside CONNECT-TO sends this letter (system D101, then
 * 04 02 00 01) and hangs on our row until answered. The real D100 answers the same letter from
 * D102/D103 (records 218, 378, 446) with the plain accept from its *TADADM port. */
static void status_question_is_answered_without_a_session(void)
{
    uint32_t now = bring_up(0x0096, 0x0010);
    uint8_t raw[80];
    XmsgDatagram dg;
    uint8_t *body;
    size_t cap;
    int len =
        test_hex_to_bytes("2113000E0065006400960400D97F210086E400650000006409E8040000410014FF07"
                          "2A54414441444D00FE044431303104020001",
                          raw, sizeof raw);
    CHECK_EQ(len, 52); /* 14 header + 14 sub-header + 24 body */
    responder_start(false);
    s_status_cb = true; /* the application says: 3 of 8 in use */
    CHECK_EQ(xmsg_dg_parse(raw, (size_t)len, &dg), XMSG_OK);
    CHECK(tad_responder_is_connect(&dg));
    CHECK(tad_responder_is_status_query(&dg));
    dg.sub.dst_system = 102;
    CHECK_EQ(xmsg_node_send_begin(&s_a, &body, &cap), XMSG_OK);
    memcpy(body, dg.body, dg.body_len);
    CHECK_EQ(xmsg_node_send_commit(&s_a, 102, &dg.sub, dg.body_len, now, NULL), XMSG_OK);
    now = run(now, 300);
    CHECK_EQ(s_offer_rc, 1);
    CHECK_EQ(s_app.calls, 0); /* not a call */
    CHECK_EQ(s_resp.state, TAD_RESP_IDLE);
    CHECK_EQ(s_resp.stats.status_queries, 1);
    CHECK_EQ(s_seen.count, 1);
    if (s_seen.count == 1)
    {
        const uint8_t *f = s_seen.frames[0];
        CHECK_BYTES(f + 14, 4, "21008640");
        CHECK_EQ(xmsg_be16_read(f + OFF_DST_PORT), 0x09E8);
        CHECK_EQ(xmsg_be16_read(f + OFF_SRC_PORT), 0x0156);
        CHECK_BYTES(f + 26, s_seen.lens[0] - 26,
                    "040000410008010200030202000"
                    "5");
    }
    /* Plain LI-SYS at D100's @ prompt (live 2026-11-10 19:09:27) sends parameter 4 = 0: also
     * a status question, never a call. */
    len = test_hex_to_bytes("2113000E0065006400960400D97F210086E400650000006409E8040000410014FF07"
                            "2A54414441444D00FE044431303104020000",
                            raw, sizeof raw);
    CHECK_EQ(len, 52);
    CHECK_EQ(xmsg_dg_parse(raw, (size_t)len, &dg), XMSG_OK);
    CHECK(tad_responder_is_status_query(&dg));
    /* A call letter (no parameter 4) is not a status question. */
    len = test_hex_to_bytes(k_tad_asker_frames[0].hex, raw, sizeof raw);
    CHECK_EQ(xmsg_dg_parse(raw, (size_t)len, &dg), XMSG_OK);
    CHECK(tad_responder_is_connect(&dg));
    CHECK(!tad_responder_is_status_query(&dg));
    (void)now;
}

/* ------------------------------------------------------------------ a console user's keys */

/* The bridge path end to end: a telnet user on a ConsoleHost, its onward session our own
 * asker on node 100, the responder on 102 standing in for D100. One onward_write call is one
 * tad_asker_write (as host_tad_telnet.c does since 2026-11-10). */
static ConsoleHost s_ch;
static ConsoleSession s_cs[1];
static char s_cl_out[1024];
static size_t s_cl_len;

static int cl_write(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    if (s_cl_len + n <= sizeof s_cl_out)
    {
        memcpy(s_cl_out + s_cl_len, b, n);
        s_cl_len += n;
    }
    return (int)n;
}

static void cl_close(void *ctx, ConsoleClose why)
{
    (void)ctx;
    (void)why;
}

static int cs_resolve(void *ctx, const char *system, uint16_t *number)
{
    (void)ctx;
    (void)system;
    *number = 102;
    return 0;
}

static int cs_open(void *ctx, int place, const char *system, uint16_t number, const char *tad_user)
{
    (void)ctx;
    (void)place;
    (void)system;
    (void)number;
    (void)tad_user;
    return 0; /* the asker is already connected by the test */
}

static uint32_t s_cs_now;

static int cs_write(void *ctx, int place, const uint8_t *b, size_t n)
{
    int took;
    (void)ctx;
    (void)place;
    if (!tad_asker_may_type(&s_asker))
    {
        return 0;
    }
    took = tad_asker_write(&s_asker, b, n, s_cs_now);
    return (took > 0) ? took : 0;
}

static void cs_hangup(void *ctx, int place)
{
    (void)ctx;
    (void)place;
}

/* Run the pair and the console poll together. */
static uint32_t run_console(uint32_t now, int ms)
{
    for (int i = 0; i < ms; i++)
    {
        now++;
        s_cs_now = now;
        pump(now);
        console_host_poll(&s_ch, now);
    }
    return now;
}

static void console_function_key_follows_break_setting(void)
{
    static const uint8_t help[] = {0x1B, 0x5B, 0x34, 0x36, 0x5F}; /* TDV HELP, measured */
    static const uint8_t wire[] = {0x1B, 0xDB, 0xB4, 0x36, 0x5F}; /* even parity in bit 7 */
    TadResponderConfig rcfg;
    TadResponderCallbacks rcb = {&s_app,      app_incoming, app_state,
                                 app_text_in, app_escape,   app_status};
    ConsoleConfig cfg;
    ConsoleLine l;
    ConsoleSession *s;
    uint32_t now = bring_up(0, 0);

    memset(&s_app, 0, sizeof s_app);
    tad_responder_config_default(&rcfg);
    rcfg.strip_parity = false; /* the BDAT bytes exactly as on the wire */
    CHECK_EQ(tad_responder_init(&s_resp, &rcfg, &rcb), XMSG_OK);
    now = peer_session(now);
    CHECK_EQ(tad_responder_echo(&s_resp, 0x01), XMSG_OK);
    CHECK_EQ(tad_responder_grant_input(&s_resp), XMSG_OK);
    now = run(now, 300);
    CHECK(tad_asker_may_type(&s_asker));

    console_config_default(&cfg);
    cfg.mode = CONSOLE_MODE_DIRECT;
    cfg.direct_system = "D102";
    cfg.services.resolve = cs_resolve;
    cfg.services.onward_open = cs_open;
    cfg.services.onward_write = cs_write;
    cfg.services.onward_hangup = cs_hangup;
    CHECK_EQ(console_host_init(&s_ch, &cfg, s_cs, 1), 0);
    memset(&l, 0, sizeof l);
    l.kind = "telnet";
    l.write = cl_write;
    l.close = cl_close;
    s = console_host_open(&s_ch, &l, now);
    CHECK(s != NULL);
    console_onward_state(s, CONSOLE_ONWARD_RUNNING, 0);
    s_cl_len = 0;

    /* The host has sent no BMMX, so its break setting is SINTRAN's default (1: control
     * characters break, tad_asker.h). The key's ESC ends the first BDAT; the rest goes after the
     * next grant. Nothing is echoed to the user. */
    s_cs_now = now;
    console_input(s, help, sizeof help, now);
    now = run_console(now, 300);
    CHECK_EQ(s_app.inputs, 1);
    CHECK(s_app.typed_len == 1 && s_app.typed[0] == 0x1B);
    s_app.typed_len = 0;
    CHECK_EQ(tad_responder_grant_input(&s_resp), XMSG_OK);
    now = run_console(now, 300);
    CHECK_EQ(s_app.inputs, 2);
    CHECK(s_app.typed_len == sizeof wire - 1 &&
          memcmp(s_app.typed, wire + 1, sizeof wire - 1) == 0);
    CHECK_EQ(s_cl_len, 0);

    /* No grant: nothing goes; the key waits whole in the console until the host grants. */
    s_app.typed_len = 0;
    console_input(s, help, 2, now);
    now = run_console(now, 20);
    console_input(s, &help[2], 3, now);
    now = run_console(now, 300);
    CHECK_EQ(s_app.inputs, 2);
    CHECK_EQ(s_app.typed_len, 0);
    CHECK_EQ(tad_responder_grant_input(&s_resp), XMSG_OK);
    now = run_console(now, 300);
    CHECK_EQ(s_app.inputs, 3);
    CHECK(s_app.typed_len == 1 && s_app.typed[0] == 0x1B);
    CHECK_EQ(s_cl_len, 0);
    console_line_lost(s);
}

int main(void)
{
    printf("[tad_responder]\n");
    host_chains_are_laid_out_like_the_host();
    replay_against_the_real_asker();
    our_asker_talks_to_our_responder();
    output_waits_for_the_askers_answers();
    refusal();
    status_question_is_answered_without_a_session();
    console_function_key_follows_break_setting();
    TEST_DONE();
}
