/*
 * test_tad_asker.c - the XROUT letter, the TAD chain, and a terminal session replayed against a
 * real SINTRAN host's captured frames.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * The replay runs two real nodes back to back on the in-memory pair. Node 100 carries our asker.
 * Node 102 plays the host: it sends the CAPTURED host frames, at their captured Flags1, each one
 * when the asker has sent the frame that triggered it on the real machines. The asker's node
 * starts its count to 102 at the captured 0x00F8, so every frame our asker sends must equal the
 * frame the real D100 sent, byte for byte, header checksum included. The only bytes that differ
 * are our port word, which the node mints: the test patches it into the expected frames and into
 * the host frames it replays.
 */
#include "libxmsg/tad/tad_asker.h"
#include "libxmsg/tad/tad_codec.h"
#include "libxmsg/xmsg/xmsg_node.h"
#include "libxmsg/xmsg_endian.h"
#include "libxmsg/xrout/xrout_letter.h"

#include "pair/test_pair.h"
#include "test_util.h"
#include "vectors/tad_connect_d100_d102.h"

#define N_ASKER (sizeof k_tad_asker_frames / sizeof k_tad_asker_frames[0])
#define N_HOST  (sizeof k_tad_host_frames / sizeof k_tad_host_frames[0])

/* Sub-header offsets inside a frame (14-byte SINTRAN header first). */
#define OFF_DST_PORT 20
#define OFF_SRC_PORT 24

/* ------------------------------------------------------------------ letters */

static void letter_matches_the_capture(void)
{
    uint8_t out[64];
    size_t n = xrout_letter_build(out, sizeof out, "*TADADM", "D102");
    /* Body of the captured D100 connect letter (record 244). */
    CHECK_BYTES(out, n, "00410010FF072A54414441444D00FE0444313032");
    /* Too small a buffer gives nothing rather than a cut letter. */
    CHECK_EQ(xrout_letter_build(out, 19, "*TADADM", "D102"), 0);
    CHECK_EQ(xrout_letter_build(out, 20, "*TADADM", "D102"), 20);
}

static void accept_and_refusal_are_told_apart(void)
{
    uint8_t body[32];
    XroutMessage m;
    /* Captured accept (record 294): integer parameters 1 = 0 and 2 = 10. */
    int len = test_hex_to_bytes("00410008010200000202000A", body, sizeof body);
    CHECK_EQ(xrout_parse(body, (size_t)len, &m), XMSG_OK);
    CHECK(xrout_reply_accepted(&m));
    CHECK_EQ(m.param_count, 2);
    CHECK_EQ(xrout_param_int(&m, 1), 0);
    CHECK_EQ(xrout_param_int(&m, 2), 10);
    CHECK_EQ(xrout_param_int(&m, 3), -1);
    /* The same reply with the service byte overwritten by XRUNN (2): refused. */
    body[1] = 0x02;
    CHECK_EQ(xrout_parse(body, (size_t)len, &m), XMSG_OK);
    CHECK(!xrout_reply_accepted(&m));
    CHECK_EQ(m.service, 2);
    CHECK(strstr(xrout_error_name(m.service), "no such name") != NULL);
    /* A letter with three parameters, as D102 sent one to D100 (record 218): fill bytes are
     * skipped and the strings come back with their numbers. */
    len = test_hex_to_bytes("00410014FF072A54414441444D00FE044431303004020001", body, sizeof body);
    CHECK_EQ(xrout_parse(body, (size_t)len, &m), XMSG_OK);
    CHECK_EQ(m.param_count, 3);
    CHECK(m.params[0].is_string && m.params[0].number == 1 && m.params[0].len == 7);
    CHECK(memcmp(m.params[0].data, "*TADADM", 7) == 0);
    CHECK(m.params[1].is_string && m.params[1].number == 2);
    CHECK(memcmp(m.params[1].data, "D100", 4) == 0);
    CHECK_EQ(xrout_param_int(&m, 4), 1);
    /* Short and cut messages. */
    CHECK_EQ(xrout_parse(body, 3, &m), XMSG_EINVAL);
    CHECK_EQ(xrout_parse(body, 10, &m), XMSG_EPROTO); /* the first string runs past the end */
}

/* ------------------------------------------------------------------ the chain */

static void chain_reads_host_fill_and_cuts(void)
{
    uint8_t buf[64];
    TadReader r;
    TadMessage m;
    /* Captured port assignment chain (record 296), host fill bytes included. */
    int len = test_hex_to_bytes("0007050000 6604C2 1F034C0000 00 0B020304 15020108 FF00", buf,
                                sizeof buf);
    static const uint8_t want[] = {0x07, 0x1F, 0x0B, 0x15, 0xFF};
    size_t got = 0;
    tad_reader_init(&r, buf, (size_t)len);
    while (tad_reader_next(&r, &m))
    {
        CHECK(got < sizeof want);
        if (got < sizeof want)
        {
            CHECK_EQ(m.opcode, want[got]);
        }
        if (m.opcode == 0x07)
        {
            CHECK_EQ(m.len, 5);
            CHECK_EQ(xmsg_be16_read(m.data + 3), 0x04C2);
        }
        got++;
    }
    CHECK_EQ(got, sizeof want);
    CHECK(tad_chain_has(buf, (size_t)len, TAD_OP_LUN));
    CHECK(!tad_chain_has(buf, (size_t)len, TAD_OP_RFI));

    /* A count that runs past the end: reported with what is there, then the end. */
    len = test_hex_to_bytes("01054142", buf, sizeof buf);
    tad_reader_init(&r, buf, (size_t)len);
    CHECK(tad_reader_next(&r, &m));
    CHECK_EQ(m.count, 5);
    CHECK_EQ(m.len, 2);
    CHECK(!tad_reader_next(&r, &m));
    /* An opcode with no count byte. */
    tad_reader_init(&r, buf, 1);
    CHECK(!tad_reader_next(&r, &m));
}

static void chain_writes_packed_like_the_asker(void)
{
    uint8_t buf[32];
    TadWriter w;
    static const uint8_t tmod = 0x08;
    static const uint8_t ttyp[2] = {0, 0};
    static const uint8_t desc = 0x1B;
    static const uint8_t opsv[3] = {0x4C, 0x01, 0x04};
    tad_writer_init(&w, buf, sizeof buf);
    CHECK_EQ(tad_writer_add(&w, TAD_OP_TMOD, &tmod, 1), XMSG_OK);
    CHECK_EQ(tad_writer_add(&w, TAD_OP_TTYP, ttyp, 2), XMSG_OK);
    CHECK_EQ(tad_writer_add(&w, TAD_OP_DESC, &desc, 1), XMSG_OK);
    CHECK_EQ(tad_writer_add(&w, TAD_OP_OPSV, opsv, 3), XMSG_OK);
    /* Captured terminal setup chain (record 249). */
    CHECK_BYTES(buf, tad_writer_length(&w), "0C01080D0200000F011B1F034C0104");

    tad_writer_init(&w, buf, 4);
    CHECK_EQ(tad_writer_add(&w, TAD_OP_BDAT, (const uint8_t *)"ab", 2), XMSG_OK);
    CHECK_EQ(tad_writer_add(&w, TAD_OP_RFI, NULL, 0), XMSG_ENOSPC);
    CHECK_EQ(tad_writer_length(&w), 0); /* an overflowed chain is never half sent */
    CHECK_EQ(tad_writer_add(&w, TAD_OP_BDAT, NULL, 300), XMSG_EINVAL);
}

static void keystrokes_get_even_parity(void)
{
    const char *typed = "sys\r";
    uint8_t out[4];
    for (int i = 0; i < 4; i++)
    {
        out[i] = tad_even_parity((uint8_t)typed[i]);
    }
    CHECK_BYTES(out, 4, "F3F9F38D"); /* record 257 */
    for (int c = 0; c < 256; c++)
    {
        uint8_t p = tad_even_parity((uint8_t)c);
        int ones = 0;
        for (int b = 0; b < 8; b++)
        {
            ones += (p >> b) & 1;
        }
        CHECK_EQ(ones & 1, 0);
        CHECK_EQ(p & 0x7F, c & 0x7F);
    }
}

/* ------------------------------------------------------------------ two nodes */

typedef struct Store
{
    uint16_t next_to_102;
} Store;

static int store_load(void *ctx, uint16_t node, uint16_t *next)
{
    if (node != 102)
    {
        return -1;
    }
    *next = ((Store *)ctx)->next_to_102;
    return 0;
}

static int store_save(void *ctx, uint16_t node, uint16_t next)
{
    if (node == 102)
    {
        ((Store *)ctx)->next_to_102 = next;
    }
    return 0;
}

#define MAX_SEEN 64

typedef struct Host
{
    uint8_t frames[MAX_SEEN][640]; /* asker frames as received, re-serialised */
    size_t lens[MAX_SEEN];
    uint16_t flags1[MAX_SEEN];
    size_t count;
} Host;

typedef struct Watch
{
    uint8_t text[8192];
    size_t text_len;
    uint8_t eckm[32];
    size_t eckm_count;
    TadAskerState states[16];
    int errors[16];
    size_t state_count;
    int gate_opens;
} Watch;

static XmsgNode s_a; /* the asker's machine, node 100 */
static XmsgNode s_b; /* the host, node 102 */
static TestPair s_pair;
static TestPairPort s_port_a;
static TestPairPort s_port_b;
static Store s_store_a;
static Host s_host;
static Watch s_watch;
static TadAsker s_asker;

static void host_keep(void *ctx, const XmsgDatagram *dg)
{
    Host *h = (Host *)ctx;
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

static void host_on_letter(void *ctx, const XmsgDatagram *dg, uint32_t now_ms)
{
    (void)now_ms;
    host_keep(ctx, dg);
}

static void w_text(void *ctx, const uint8_t *bytes, size_t len)
{
    Watch *w = (Watch *)ctx;
    if (w->text_len + len <= sizeof w->text)
    {
        memcpy(w->text + w->text_len, bytes, len);
        w->text_len += len;
    }
}

static void w_state(void *ctx, TadAskerState st, int error)
{
    Watch *w = (Watch *)ctx;
    if (w->state_count < 16)
    {
        w->states[w->state_count] = st;
        w->errors[w->state_count] = error;
        w->state_count++;
    }
}

static void w_gate(void *ctx, bool open)
{
    if (open)
    {
        ((Watch *)ctx)->gate_opens++;
    }
}

static void w_echo(void *ctx, uint8_t eckm)
{
    Watch *w = (Watch *)ctx;
    if (w->eckm_count < sizeof w->eckm)
    {
        w->eckm[w->eckm_count++] = eckm;
    }
}

static bool s_tcp_mode;
static TestTcpPair s_tcp;
static TestTcpPort s_tcp_a;
static TestTcpPort s_tcp_b;

/* The next moment: one simulated millisecond on the in-memory pair, the real clock on TCP. */
static uint32_t step(uint32_t now)
{
    if (!s_tcp_mode)
    {
        return now + 1;
    }
    test_sleep_ms(1);
    return test_now_ms();
}

static void pump(uint32_t now)
{
    uint8_t chunk[256];
    size_t n;
    xmsg_node_tick(&s_a, now);
    xmsg_node_tick(&s_b, now);
    if (s_tcp_mode)
    {
        int r;
        while ((r = test_tcp_read(s_tcp.sock[0], chunk, sizeof chunk)) > 0)
        {
            xmsg_node_feed(&s_a, chunk, (size_t)r, now);
        }
        while ((r = test_tcp_read(s_tcp.sock[1], chunk, sizeof chunk)) > 0)
        {
            xmsg_node_feed(&s_b, chunk, (size_t)r, now);
        }
        return;
    }
    while ((n = test_pair_read(&s_pair, 0, chunk, sizeof chunk)) > 0)
    {
        xmsg_node_feed(&s_a, chunk, n, now);
    }
    while ((n = test_pair_read(&s_pair, 1, chunk, sizeof chunk)) > 0)
    {
        xmsg_node_feed(&s_b, chunk, n, now);
    }
}

static uint32_t bring_up(uint16_t first_flags1)
{
    XmsgTransportPort ta;
    XmsgTransportPort tb;
    XmsgNodeConfig cfg;
    XmsgPorts ports;
    XmsgNodeCallbacks cb;
    uint32_t now = 0;

    memset(&s_host, 0, sizeof s_host);
    memset(&s_watch, 0, sizeof s_watch);
    s_store_a.next_to_102 = first_flags1;
    if (s_tcp_mode)
    {
        CHECK_EQ(test_tcp_pair_open(&s_tcp), 0);
        test_tcp_transport(s_tcp.sock[0], &s_tcp_a, &ta);
        test_tcp_transport(s_tcp.sock[1], &s_tcp_b, &tb);
        now = test_now_ms();
    }
    else
    {
        test_pair_init(&s_pair);
        test_pair_transport(&s_pair, 0, &s_port_a, &ta);
        test_pair_transport(&s_pair, 1, &s_port_b, &tb);
    }

    xmsg_node_config_default(&cfg, 100);
    cfg.lapb.t1_ms = 50;
    cfg.settle_ms = 20;
    memset(&ports, 0, sizeof ports);
    ports.transport = ta;
    ports.flags1.ctx = &s_store_a;
    ports.flags1.load = store_load;
    ports.flags1.save = store_save;
    memset(&cb, 0, sizeof cb);
    CHECK_EQ(xmsg_node_init(&s_a, &cfg, &ports, &cb), XMSG_OK);

    xmsg_node_config_default(&cfg, 102);
    cfg.lapb.t1_ms = 50;
    cfg.settle_ms = 20;
    memset(&ports, 0, sizeof ports);
    ports.transport = tb;
    memset(&cb, 0, sizeof cb);
    cb.ctx = &s_host;
    cb.on_letter = host_on_letter;
    cb.unhandled = host_keep;
    CHECK_EQ(xmsg_node_init(&s_b, &cfg, &ports, &cb), XMSG_OK);

    xmsg_node_link_start(&s_a, now);
    xmsg_node_link_start(&s_b, now);
    uint32_t up_start = now;
    while (!(xmsg_node_is_ready(&s_a) && xmsg_node_is_ready(&s_b)) && now - up_start < 5000)
    {
        now = step(now);
        pump(now);
    }
    CHECK(xmsg_node_is_ready(&s_a) && xmsg_node_is_ready(&s_b));
    return now;
}

static void asker_start(uint32_t now)
{
    TadAskerConfig cfg;
    TadAskerCallbacks cb = {&s_watch, w_text, w_state, w_gate, w_echo};
    tad_asker_config_default(&cfg, 102, "D102");
    CHECK_EQ(tad_asker_init(&s_asker, &cfg, &cb), XMSG_OK);
    CHECK_EQ(tad_asker_connect(&s_asker, &s_a, now), XMSG_OK);
}

/* Send one captured host frame from node 102, aimed at our asker's port. */
static void host_send_hex(const char *hex, uint16_t flags1, uint8_t service_override, uint32_t now)
{
    uint8_t raw[640];
    XmsgDatagram dg;
    uint8_t *body;
    size_t cap;
    int len = test_hex_to_bytes(hex, raw, sizeof raw);
    CHECK(len > 28);
    raw[OFF_DST_PORT] = (uint8_t)(s_asker.base.local_port >> 8);
    raw[OFF_DST_PORT + 1] = (uint8_t)s_asker.base.local_port;
    CHECK_EQ(xmsg_dg_parse(raw, (size_t)len, &dg), XMSG_OK);
    CHECK_EQ(xmsg_node_send_begin(&s_b, &body, &cap), XMSG_OK);
    memcpy(body, dg.body, dg.body_len);
    if (service_override != 0)
    {
        body[1] = service_override;
    }
    CHECK_EQ(xmsg_node_send_repeat(&s_b, 100, &dg.sub, dg.body_len, flags1, now), XMSG_OK);
}

/* The BDAT data of a captured asker frame, parity stripped: what the user typed. */
static size_t typed_in(const char *hex, uint8_t *out)
{
    uint8_t raw[640];
    int len = test_hex_to_bytes(hex, raw, sizeof raw);
    TadReader r;
    TadMessage m;
    tad_reader_init(&r, raw + 32, (size_t)len - 32);
    if (!tad_reader_next(&r, &m) || m.opcode != TAD_OP_BDAT)
    {
        return 0;
    }
    for (size_t i = 0; i < m.len; i++)
    {
        out[i] = (uint8_t)(m.data[i] & 0x7Fu);
    }
    return m.len;
}

static bool is_bdat(const char *hex)
{
    uint8_t raw[640];
    int len = test_hex_to_bytes(hex, raw, sizeof raw);
    return len > 33 && raw[32] == TAD_OP_BDAT;
}

static void replay_of_a_real_session(bool over_tcp)
{
    s_tcp_mode = over_tcp;
    uint32_t now = bring_up(0x00F8);
    size_t checked = 0;
    size_t host_next = 0;
    bool repeated_accept = false;
    uint8_t want[640];
    size_t host_text_len = 0;
    uint8_t host_text[8192];

    asker_start(now);
    uint32_t start = now;
    while (now - start < 20000 && s_asker.state != TAD_ASKER_CLOSED &&
           s_asker.state != TAD_ASKER_FAILED)
    {
        now = step(now);
        pump(now);

        /* Compare every new asker frame with the capture. */
        while (checked < s_host.count && checked < N_ASKER)
        {
            int wl = test_hex_to_bytes(k_tad_asker_frames[checked].hex, want, sizeof want);
            want[OFF_SRC_PORT] = (uint8_t)(s_asker.base.local_port >> 8);
            want[OFF_SRC_PORT + 1] = (uint8_t)s_asker.base.local_port;
            CHECK_EQ(s_host.lens[checked], (size_t)wl);
            if (s_host.lens[checked] != (size_t)wl ||
                memcmp(s_host.frames[checked], want, (size_t)wl) != 0)
            {
                printf("  asker frame %u (Flags1 0x%04X) differs from the capture\n",
                       (unsigned)checked, k_tad_asker_frames[checked].flags1);
                CHECK(false);
            }
            checked++;
        }

        /* Host frames whose trigger has been sent and seen. */
        while (host_next < N_HOST && checked > 0 &&
               s_host.flags1[checked - 1] >= k_tad_host_frames[host_next].after)
        {
            host_send_hex(k_tad_host_frames[host_next].hex, k_tad_host_frames[host_next].flags1, 0,
                          now);
            host_next++;
            /* Once, send the accept again at its old number: a repeat must change nothing. */
            if (host_next == 3 && !repeated_accept)
            {
                host_send_hex(k_tad_host_frames[0].hex, k_tad_host_frames[0].flags1, 0, now);
                repeated_accept = true;
            }
        }

        /* Type what the D100 user typed, when the next captured asker frame is text. */
        if (checked < N_ASKER && s_host.count == checked && tad_asker_may_type(&s_asker) &&
            is_bdat(k_tad_asker_frames[checked].hex))
        {
            uint8_t keys[256];
            size_t nk = typed_in(k_tad_asker_frames[checked].hex, keys);
            CHECK_EQ(tad_asker_write(&s_asker, keys, nk, now), (int)nk);
            CHECK_EQ(tad_asker_write(&s_asker, keys, nk, now), XMSG_EAGAIN); /* one per grant */
        }
    }

    CHECK_EQ(checked, N_ASKER);
    CHECK_EQ(host_next, N_HOST);
    CHECK_EQ(s_asker.state, TAD_ASKER_CLOSED);
    CHECK_EQ(s_asker.stats.repeats_in, 1);
    CHECK_EQ(s_asker.session_port, 0x04C2);
    CHECK_EQ(s_asker.tadadm_port, 0x0156);
    CHECK_EQ(s_asker.lun[1], 0x04);
    CHECK(!s_asker.base.attached);

    /* The last frame is our DCON, answering the host's 0xFD, at the next number. */
    CHECK_EQ(s_host.count, N_ASKER + 1);
    if (s_host.count == N_ASKER + 1)
    {
        const uint8_t *f = s_host.frames[N_ASKER];
        CHECK_EQ(s_host.flags1[N_ASKER], 0x0110);
        CHECK_BYTES(f + 14, 4, "21008294");                 /* bare control, role 0x94 */
        CHECK_EQ(xmsg_be16_read(f + OFF_DST_PORT), 0x04C2); /* to the session port */
        CHECK_BYTES(f + 26, s_host.lens[N_ASKER] - 26, "0008000000020900");
    }

    /* Host text reached the application exactly as sent: every BDAT, in order. */
    for (size_t i = 0; i < N_HOST; i++)
    {
        uint8_t raw[640];
        int len = test_hex_to_bytes(k_tad_host_frames[i].hex, raw, sizeof raw);
        uint16_t lead;
        const uint8_t *chain;
        size_t clen;
        TadReader r;
        TadMessage m;
        CHECK_EQ(tad_body_split(raw + 28, (size_t)len - 28, &lead, &chain, &clen), XMSG_OK);
        if (k_tad_host_frames[i].flags1 < 0x0133)
        {
            continue; /* accept, port assignment, DUMM, ESRS: not terminal text */
        }
        tad_reader_init(&r, chain, clen);
        while (tad_reader_next(&r, &m))
        {
            if (m.opcode == TAD_OP_BDAT && host_text_len + m.len <= sizeof host_text)
            {
                memcpy(host_text + host_text_len, m.data, m.len);
                host_text_len += m.len;
            }
        }
    }
    CHECK_EQ(s_watch.text_len, host_text_len);
    CHECK(memcmp(s_watch.text, host_text, host_text_len) == 0);
    CHECK(s_watch.text_len > 1500); /* the LIST-FILES output came through */

    /* Echo instructions in the order the host gave them: on at the banner, off for the password,
     * on again after it. */
    CHECK(s_watch.eckm_count >= 3);
    CHECK_EQ(s_watch.eckm[0], 0x01);
    CHECK_EQ(s_watch.eckm[1], 0xFF);
    CHECK_EQ(s_watch.eckm[2], 0x01);

    /* States in order. */
    static const TadAskerState seq[] = {
        TAD_ASKER_CONNECTING, TAD_ASKER_ACCEPTED, TAD_ASKER_PORT_ASSIGNED, TAD_ASKER_TERMINAL_SETUP,
        TAD_ASKER_RUNNING,    TAD_ASKER_CLOSING,  TAD_ASKER_CLOSED};
    CHECK_EQ(s_watch.state_count, sizeof seq / sizeof seq[0]);
    for (size_t i = 0; i < s_watch.state_count && i < sizeof seq / sizeof seq[0]; i++)
    {
        CHECK_EQ(s_watch.states[i], seq[i]);
    }
    CHECK_EQ(s_a.stats.bad_headers + s_b.stats.bad_headers, 0);
    printf("  replay over %s: %u asker frames compared with the capture, %u host frames, "
           "%u text bytes, %u ms\n",
           over_tcp ? "loopback TCP" : "the in-memory pair", (unsigned)checked, (unsigned)host_next,
           (unsigned)s_watch.text_len, (unsigned)(now - start));
    if (over_tcp)
    {
        test_tcp_pair_close(&s_tcp);
    }
}

static void refused_letter_fails_with_the_xrout_code(void)
{
    uint32_t now = bring_up(0x00F8);
    asker_start(now);
    for (int i = 0; i < 200 && s_host.count == 0; i++)
    {
        pump(++now);
    }
    CHECK_EQ(s_host.count, 1);
    host_send_hex(k_tad_host_frames[0].hex, k_tad_host_frames[0].flags1, 0x04, now); /* XRNSP */
    for (int i = 0; i < 200 && s_asker.state == TAD_ASKER_CONNECTING; i++)
    {
        pump(++now);
    }
    CHECK_EQ(s_asker.state, TAD_ASKER_FAILED);
    CHECK_EQ(s_asker.error, 4);
    CHECK(!s_asker.base.attached);
    CHECK_EQ(s_watch.errors[s_watch.state_count - 1], 4);
    for (int i = 0; i < 200; i++)
    {
        pump(++now);
    }
    CHECK_EQ(s_host.count, 1); /* nothing more was sent */
}

static void silent_host_times_out(void)
{
    uint32_t now = bring_up(0x00F8);
    asker_start(now);
    uint32_t start = now;
    while (s_asker.state == TAD_ASKER_CONNECTING && now - start < 40000)
    {
        pump(++now);
    }
    CHECK_EQ(s_asker.state, TAD_ASKER_FAILED);
    CHECK_EQ(s_asker.error, XMSG_ETIMEDOUT);
    CHECK(now - start >= 30000 && now - start < 30010);
    CHECK_EQ(s_host.count, 1);
}

static void xense_resends_and_other_errors_fail(void)
{
    uint32_t now = bring_up(0x00F8);
    /* Hold the host's ACKs back by not pumping, so the letter stays unacknowledged. */
    TadAskerConfig cfg;
    TadAskerCallbacks cb = {&s_watch, w_text, w_state, w_gate, w_echo};
    tad_asker_config_default(&cfg, 102, "D102");
    CHECK_EQ(tad_asker_init(&s_asker, &cfg, &cb), XMSG_OK);
    CHECK_EQ(tad_asker_connect(&s_asker, &s_a, now), XMSG_OK);
    CHECK_EQ(s_asker.inflight, TAD_SEND_LETTER);
    uint16_t first = s_asker.inflight_flags1;
    CHECK_EQ(first, 0x00F8);

    /* The node reports XENSE for our letter: the asker sends the letter again. */
    s_asker.base.ops->on_error(&s_asker.base, XMSG_XE_XENSE, first);
    CHECK_EQ(s_asker.inflight, TAD_SEND_NONE);
    CHECK(s_asker.base.wants_tx);
    pump(++now);
    CHECK_EQ(s_asker.inflight, TAD_SEND_LETTER);
    CHECK_EQ(s_asker.stats.xense_resends, 1);
    /* An error for a number we are not waiting on is not ours. */
    s_asker.base.ops->on_error(&s_asker.base, -19, 0x1234);
    CHECK_EQ(s_asker.state, TAD_ASKER_CONNECTING);
    /* XEIMA for the one in flight ends the session. */
    s_asker.base.ops->on_error(&s_asker.base, -19, s_asker.inflight_flags1);
    CHECK_EQ(s_asker.state, TAD_ASKER_FAILED);
    CHECK_EQ(s_asker.error, -19);
    CHECK(!s_asker.base.attached);
}

static void typing_rules(void)
{
    TadAsker a;
    TadAskerConfig cfg;
    const uint8_t keys[2] = {0xC1, 0x0D};
    tad_asker_config_default(&cfg, 102, "D102");
    CHECK_EQ(tad_asker_init(&a, &cfg, NULL), XMSG_OK);
    CHECK_EQ(tad_asker_write(&a, keys, 2, 0), XMSG_ESTATE); /* not running */
    a.state = TAD_ASKER_RUNNING;                            /* stand-alone, no node */
    CHECK_EQ(tad_asker_write(&a, keys, 2, 0), XMSG_EAGAIN); /* no grant */
    a.rfi_open = true;
    CHECK_EQ(tad_asker_write(&a, keys, 2, 0), 2);
    CHECK_BYTES(a.line, a.line_len, "418D"); /* 'A' already even; CR gains bit 7 */
    CHECK(!tad_asker_may_type(&a));

    cfg.keystroke_parity = false;
    CHECK_EQ(tad_asker_init(&a, &cfg, NULL), XMSG_OK);
    a.state = TAD_ASKER_RUNNING;
    a.rfi_open = true;
    CHECK_EQ(tad_asker_write(&a, keys, 2, 0), 2);
    CHECK_BYTES(a.line, a.line_len, "C10D"); /* bytes as given */

    uint8_t many[300];
    memset(many, 'x', sizeof many);
    CHECK_EQ(tad_asker_init(&a, &cfg, NULL), XMSG_OK);
    a.state = TAD_ASKER_RUNNING;
    a.rfi_open = true;
    CHECK_EQ(tad_asker_write(&a, many, sizeof many, 0), 255);

    cfg.system_name = "";
    CHECK_EQ(tad_asker_init(&a, &cfg, NULL), XMSG_EINVAL);
    cfg.system_name = "A-NAME-FAR-TOO-LONG";
    CHECK_EQ(tad_asker_init(&a, &cfg, NULL), XMSG_EINVAL);
}

int main(void)
{
    printf("[tad_asker]\n");
    letter_matches_the_capture();
    accept_and_refusal_are_told_apart();
    chain_reads_host_fill_and_cuts();
    chain_writes_packed_like_the_asker();
    keystrokes_get_even_parity();
    typing_rules();
    replay_of_a_real_session(false);
    CHECK_EQ(test_tcp_startup(), 0);
    replay_of_a_real_session(true);
    s_tcp_mode = false;
    refused_letter_fails_with_the_xrout_code();
    silent_host_times_out();
    xense_resends_and_other_errors_fail();
    TEST_DONE();
}
