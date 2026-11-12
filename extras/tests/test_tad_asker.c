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
    cfg.reject_closed_ports = false; /* a tap: it takes every datagram through unhandled */
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
    TadAskerCallbacks cb = {&s_watch, w_text, w_state, w_gate, w_echo, NULL, NULL};
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
    bool early_notice = false;
    uint8_t want[640];
    size_t host_text_len = 0;
    uint8_t host_text[8192];

    asker_start(now);
    uint32_t start = now;
    /* Until the capture is used up: every host frame sent, every asker frame compared, the
     * closing notice taken and nothing of ours left unacknowledged. The session itself stays
     * open (a notice ends nothing); the hang-up below ends it. */
    while (now - start < 20000 && s_asker.state != TAD_ASKER_CLOSED &&
           s_asker.state != TAD_ASKER_FAILED &&
           !(host_next == N_HOST && checked == N_ASKER && s_asker.stats.host_notices == 2 &&
             s_asker.inflight == TAD_SEND_NONE))
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
            /* Once, right after the prompt that follows the login OK, send the host's session
             * notice (its 0xFD frame) at a number of its own: another captured session has one
             * exactly there (asker 100 port 02E3, record 1327) and goes on. It must end nothing
             * and draw no reply, so every later asker frame still matches the capture. */
            if (k_tad_host_frames[host_next - 1].flags1 == 0x0139 && !early_notice)
            {
                host_send_hex(k_tad_host_frames[N_HOST - 1].hex, 0x0200, 0, now);
                early_notice = true;
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
    /* The host's last frame is its session notice (0xFD after --EXIT--). It ends nothing: the
     * session is still running and nothing more was sent. */
    CHECK(early_notice);
    CHECK_EQ(s_asker.stats.host_notices, 2); /* the early one and the one after --EXIT-- */
    /* SYCN 000B came once, with --EXIT--; the other SYCN values of the login ladder did not
     * count. The asker's reply to it (a DUMM) matched the captured asker frame above. */
    CHECK_EQ(s_asker.stats.logouts, 1);
    CHECK_EQ(s_asker.state, TAD_ASKER_RUNNING);
    CHECK_EQ(s_host.count, N_ASKER);
    /* The user is done: hang up. The DCON is acknowledged and the session closes. */
    CHECK_EQ(tad_asker_hangup(&s_asker, now), XMSG_OK);
    for (int i = 0; i < 2000 && s_asker.state != TAD_ASKER_CLOSED; i++)
    {
        now = step(now);
        pump(now);
    }
    CHECK_EQ(s_asker.state, TAD_ASKER_CLOSED);
    CHECK_EQ(s_asker.stats.repeats_in, 1);
    CHECK_EQ(s_asker.session_port, 0x04C2);
    CHECK_EQ(s_asker.tadadm_port, 0x0156);
    CHECK_EQ(s_asker.lun[1], 0x04);
    CHECK(!s_asker.base.attached);

    /* The last frame is our DCON, at the next number, in the shape of the captured asker DCONs
     * (records 1260, 803, 1296, 1519 of the capture file: 0x82, role 0x94, XMCSM 0008, 09 00). */
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

/* A call to a system beyond the link neighbour: the letter carries the TARGET's number in the
 * SINTRAN header and in the sub-header, as D102's letter to D103 through D100 does (NDInsight
 * captures ARCHIVE-2026-07, ethernet-hdlc-ROUTE-THROUGH-WORKING-102-via-100-to-103-2026-08-01
 * frame #3: 21 13 000E 0067 0066 ... | 21 00 86 E4 0067 0000 ...). Our link neighbour is 102. */
static void letter_to_a_system_beyond_the_neighbour_names_the_target(void)
{
    TadAskerConfig cfg;
    TadAskerCallbacks cb = {&s_watch, w_text, w_state, w_gate, w_echo, NULL, NULL};
    uint32_t now = bring_up(0x00F8);
    tad_asker_config_default(&cfg, 103, "D103");
    CHECK_EQ(tad_asker_init(&s_asker, &cfg, &cb), XMSG_OK);
    CHECK_EQ(tad_asker_connect(&s_asker, &s_a, now), XMSG_OK);
    for (int i = 0; i < 200 && s_host.count == 0; i++)
    {
        pump(++now);
    }
    CHECK_EQ(s_host.count, 1);
    const uint8_t *f = s_host.frames[0];
    CHECK_EQ(xmsg_be16_read(f + 4), 103);  /* SINTRAN header destination */
    CHECK_EQ(xmsg_be16_read(f + 6), 100);  /* source: us */
    CHECK_EQ(f[17], 0xE4);                 /* letter role */
    CHECK_EQ(xmsg_be16_read(f + 18), 103); /* sub-header destination system */
    CHECK_EQ(xmsg_be16_read(f + 20), 0);   /* port 0 */
    tad_asker_abort(&s_asker);
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
    TadAskerCallbacks cb = {&s_watch, w_text, w_state, w_gate, w_echo, NULL, NULL};
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

/* Measured 2026-11-09 against RetroCore D100: a host that does not know us yet answers our
 * first letter with a reachability request instead of an ACK, and drops the letter. Both sides
 * restart their count; the asker must send the letter again, now at 0000. */
static void letter_is_sent_again_after_a_host_restart(void)
{
    uint32_t now = bring_up(0x00F8);
    asker_start(now);
    CHECK_EQ(s_asker.inflight_flags1, 0x00F8);
    /* The host's request goes out before it has seen our letter. */
    CHECK_EQ(xmsg_node_send_reach(&s_b, 100, now), XMSG_OK);
    for (int i = 0; i < 400; i++)
    {
        pump(++now);
    }
    CHECK_EQ(s_asker.stats.restart_resends, 1);
    CHECK_EQ(s_host.count, 2);
    CHECK_EQ(s_host.flags1[0], 0x00F8);
    CHECK_EQ(s_host.flags1[1], 0x0000);
    CHECK_BYTES(s_host.frames[1] + 28, s_host.lens[1] - 28, k_tad_asker_frames[0].hex + 56);
    CHECK_EQ(s_asker.inflight, TAD_SEND_NONE); /* the resent letter was acknowledged */
    CHECK_EQ(s_asker.state, TAD_ASKER_CONNECTING);
    CHECK_EQ(xmsg_flags1_peek(&s_a.flags1, 102), 1);
}

/* The telnet client went away before the host assigned the session port: the hang-up is
 * remembered and the DCON is the first frame sent to the session port, so the host is not left
 * holding a session nobody uses. */
static void hangup_before_the_port_goes_out_with_the_port(void)
{
    uint32_t now = bring_up(0x00F8);
    asker_start(now);
    for (int i = 0; i < 200 && s_host.count == 0; i++)
    {
        pump(++now);
    }
    CHECK_EQ(s_host.count, 1); /* the letter */
    CHECK_EQ(tad_asker_hangup(&s_asker, now), XMSG_OK);
    CHECK_EQ(s_asker.state, TAD_ASKER_CONNECTING); /* nothing to send to yet */
    host_send_hex(k_tad_host_frames[0].hex, k_tad_host_frames[0].flags1, 0, now); /* accept */
    for (int i = 0; i < 200 && s_host.count < 2; i++)
    {
        pump(++now);
    }
    CHECK_EQ(s_host.count, 2); /* the session setup still goes: it is what asks for the port */
    host_send_hex(k_tad_host_frames[1].hex, k_tad_host_frames[1].flags1, 0, now); /* port */
    for (int i = 0; i < 400 && s_asker.state != TAD_ASKER_CLOSED; i++)
    {
        pump(++now);
    }
    CHECK_EQ(s_asker.state, TAD_ASKER_CLOSED);
    CHECK_EQ(s_host.count, 3);
    if (s_host.count == 3)
    {
        const uint8_t *f = s_host.frames[2];
        CHECK_BYTES(f + 14, 4, "21008294"); /* DCON: bare control, role 0x94 */
        CHECK_EQ(xmsg_be16_read(f + OFF_DST_PORT), s_asker.session_port);
        CHECK_BYTES(f + 26, s_host.lens[2] - 26, "0008000000020900");
    }
}

/* Hang-up while the session runs: replies waiting to go and typed text not yet sent are
 * dropped, and the DCON is the next frame. */
static void hangup_while_running_drops_what_waits(void)
{
    TadAsker a;
    TadAskerConfig cfg;
    tad_asker_config_default(&cfg, 102, "D102");
    CHECK_EQ(tad_asker_init(&a, &cfg, NULL), XMSG_OK);
    a.state = TAD_ASKER_RUNNING; /* stand-alone, no node */
    a.session_port = 0x04C2;
    a.queue[0] = (uint8_t)TAD_SEND_DUMM;
    a.queue[1] = (uint8_t)TAD_SEND_TEXT;
    a.q_head = 0;
    a.q_count = 2;
    a.line_len = 5;
    CHECK_EQ(tad_asker_hangup(&a, 0), XMSG_OK);
    CHECK_EQ(a.state, TAD_ASKER_CLOSING);
    CHECK_EQ(a.q_count, 1);
    CHECK_EQ(a.queue[a.q_head], (uint8_t)TAD_SEND_DCON);
    CHECK_EQ(a.line_len, 0);
    CHECK(!a.rfi_open);
    CHECK_EQ(tad_asker_hangup(&a, 0), XMSG_ESTATE); /* once */
    a.state = TAD_ASKER_IDLE;
    CHECK_EQ(tad_asker_hangup(&a, 0), XMSG_ESTATE);
    CHECK_EQ(tad_asker_hangup(NULL, 0), XMSG_EINVAL);
}

/* TADA asks every remote terminal "who are you" (7WHO) and waits for the answer; without one
 * it hangs on that terminal's row (live D100, 2026-11-09). The question is record 3653 of
 * RetroIO pcap-decode-sample.json (node 103 asking node 102), here sent by the host from its
 * port 0x0728; the answer must be record 3618's 7IAM byte for byte (sub-header from offset 14
 * on, and the whole body), sent back to the port that asked, with no DUMM for the question. */
static void who_are_you_is_answered_like_the_real_asker(void)
{
    /* Record 3653 as captured; the systems and the asking port are set for this test below. */
    static const char k_who[] = "2113000E0066006700260048DDA3"
                                "21008214006605DA006704DA0048"
                                "00000002FC00";
    /* Record 3618, from the sub-header on: 7IAM from D102, terminal 1, user SYSTEM. */
    static const char k_iam_body[] =
        "0000003F28000A2144313032270000000000000000000000000000000000000000000000000000000000"
        "0B020001101153595354454D2700000000000000000000FF00";
    TadAskerConfig cfg;
    TadAskerCallbacks cb = {&s_watch, w_text, w_state, w_gate, w_echo, NULL, NULL};
    uint8_t raw[64];
    char hex[2 * sizeof raw + 1];

    uint32_t now = bring_up(0x00F8);
    tad_asker_config_default(&cfg, 102, "D102");
    cfg.own_name = "D102";
    cfg.terminal_number = 1;
    cfg.user_name = "SYSTEM";
    CHECK_EQ(tad_asker_init(&s_asker, &cfg, &cb), XMSG_OK);
    CHECK_EQ(tad_asker_connect(&s_asker, &s_a, now), XMSG_OK);
    for (int i = 0; i < 200 && s_host.count == 0; i++)
    {
        pump(++now);
    }
    host_send_hex(k_tad_host_frames[0].hex, k_tad_host_frames[0].flags1, 0, now); /* accept */
    for (int i = 0; i < 200 && s_host.count < 2; i++)
    {
        pump(++now);
    }
    host_send_hex(k_tad_host_frames[1].hex, k_tad_host_frames[1].flags1, 0, now); /* port */
    for (int i = 0; i < 200; i++)
    {
        pump(++now);
    }
    CHECK_EQ(s_asker.state, TAD_ASKER_PORT_ASSIGNED);
    CHECK_EQ(s_host.count, 2); /* letter and session setup */

    int len = test_hex_to_bytes(k_who, raw, sizeof raw);
    CHECK_EQ(len, 34);
    xmsg_be16_write(raw + 18, 100);    /* to our system */
    xmsg_be16_write(raw + 22, 102);    /* from the host */
    xmsg_be16_write(raw + 24, 0x0728); /* from the host's asking port */
    for (int i = 0; i < len; i++)
    {
        static const char k_digits[] = "0123456789ABCDEF";
        hex[2 * i] = k_digits[raw[i] >> 4];
        hex[2 * i + 1] = k_digits[raw[i] & 0x0Fu];
    }
    hex[2 * len] = '\0';
    host_send_hex(hex, 0x0300, 0, now);
    for (int i = 0; i < 400; i++)
    {
        pump(++now);
    }
    CHECK_EQ(s_asker.stats.who_answers, 1);
    CHECK_EQ(s_host.count, 3); /* the 7IAM and nothing else: no DUMM for a question */
    if (s_host.count == 3)
    {
        const uint8_t *f = s_host.frames[2];
        CHECK_BYTES(f + 14, 4, "21008684"); /* setup flags, data role */
        CHECK_EQ(xmsg_be16_read(f + 18), 102);
        CHECK_EQ(xmsg_be16_read(f + OFF_DST_PORT), 0x0728);
        CHECK_EQ(xmsg_be16_read(f + 22), 100);
        CHECK_EQ(xmsg_be16_read(f + OFF_SRC_PORT), s_asker.base.local_port);
        CHECK_BYTES(f + 26, 2, "0048");
        CHECK_EQ(s_host.lens[2], 95);
        CHECK_BYTES(f + 28, s_host.lens[2] - 28, k_iam_body);
        CHECK_BYTES(f + 28, 4, "0000003F");
    }
    CHECK_EQ(s_asker.state, TAD_ASKER_PORT_ASSIGNED); /* a question changes nothing else */
}

/* The default answer names us "D" + our system number, and refuses names that do not fit. */
static void who_are_you_default_name_and_limits(void)
{
    TadAskerConfig cfg;
    TadAsker a;
    tad_asker_config_default(&cfg, 102, "D102");
    CHECK(cfg.own_name == NULL);
    CHECK_EQ(cfg.terminal_number, 1);
    CHECK(strcmp(cfg.user_name, "REMOTE") == 0);
    cfg.user_name = "12345678901234567"; /* 17: no room for the "'" */
    CHECK_EQ(tad_asker_init(&a, &cfg, NULL), XMSG_EINVAL);
    cfg.user_name = "1234567890123456"; /* 16 fits */
    CHECK_EQ(tad_asker_init(&a, &cfg, NULL), XMSG_OK);
    cfg.own_name = "123456789012345678901234567890123"; /* 33 */
    CHECK_EQ(tad_asker_init(&a, &cfg, NULL), XMSG_EINVAL);
}

/* The host drops our connect letter in silence (as SINTRAN does when its count for us is ahead
 * of ours): after ack_timeout_ms the asker asks the node to resync, the host replies, and the
 * letter goes again at Flags1 0. Here the host node is simply not run while the letter is due,
 * so the letter's acknowledgement is late; the resync then starts both counts again at 0. */
static void unanswered_letter_draws_one_resync(void)
{
    TadAskerConfig cfg;
    TadAskerCallbacks cb = {&s_watch, w_text, w_state, w_gate, w_echo, NULL, NULL};
    uint8_t chunk[256];
    size_t n;

    uint32_t now = bring_up(0x00F8);
    tad_asker_config_default(&cfg, 102, "D102");
    cfg.ack_timeout_ms = 40; /* below the LAPB T1 x N2 of the test link (50 ms x 10) */
    CHECK_EQ(tad_asker_init(&s_asker, &cfg, &cb), XMSG_OK);
    CHECK_EQ(tad_asker_connect(&s_asker, &s_a, now), XMSG_OK);
    /* Run only our side for 60 ms: the host hears nothing, so nothing is acknowledged. */
    for (int i = 0; i < 60; i++)
    {
        now++;
        xmsg_node_tick(&s_a, now);
        while ((n = test_pair_read(&s_pair, 0, chunk, sizeof chunk)) > 0)
        {
            xmsg_node_feed(&s_a, chunk, n, now);
        }
    }
    CHECK_EQ(s_asker.stats.resyncs, 1);
    CHECK(s_a.resync_pending);
    CHECK_EQ(s_asker.state, TAD_ASKER_CONNECTING);
    /* Now both sides run: the host takes the letter and the request, replies to both. */
    for (int i = 0; i < 400; i++)
    {
        pump(++now);
    }
    CHECK_EQ(s_a.stats.resyncs, 1);
    CHECK(!s_a.resync_pending);
    CHECK_EQ(s_asker.stats.resyncs, 1); /* once only */
    /* The late ACK took the letter before the reply came, so there was nothing left to send
     * again and the count simply starts at 0 (saved). Sending the unacknowledged frame again on
     * the reply is the on_peer_restart path, tested in letter_is_sent_again_after_a_host_restart;
     * our own node always ACKs, so this test cannot build a host that stays silent for good. */
    CHECK_EQ(s_store_a.next_to_102, 0);
    CHECK_EQ(s_asker.stats.restart_resends, 0);
    CHECK(s_asker.state == TAD_ASKER_CONNECTING);
    CHECK(s_host.count >= 1); /* the letter */
}

/* Feed one host terminal frame to the asker through its session entry: a body of lead word
 * 0000, the chain length, then the chain (the shape of every captured host terminal frame). */
static void give_chain(TadAsker *a, const char *chain_hex)
{
    uint8_t body[128];
    XmsgDatagram dg;
    int n = test_hex_to_bytes(chain_hex, body + 4, sizeof body - 4);
    CHECK(n > 0);
    body[0] = 0x00;
    body[1] = 0x00;
    body[2] = (uint8_t)((unsigned)n >> 8);
    body[3] = (uint8_t)n;
    memset(&dg, 0, sizeof dg);
    dg.has_subhdr = true;
    dg.body = body;
    dg.body_len = (size_t)n + 4u;
    a->base.ops->on_datagram(&a->base, &dg, 0);
}

/* The host's break setting cuts typed input (tad_asker.h). Bytes and order from the RetroServer
 * hub capture 2026-11-10 11:55:32: D100 in S3-CONFIG sends
 *   00 04 03 00 00 00 00 03 01 FF 02 00   (pad, BMMX strategy 0 BRKMAX 0, pad, ECKM FF, RFI)
 * and the real D102 asker answers with 1B, DB, B4, B8, 5F, one BDAT each. */
static void break_setting_rules(void)
{
    TadAsker a;
    TadAskerConfig cfg;
    static const uint8_t slutt[5] = {0x1B, 0x5B, 0x34, 0x38, 0x5F};
    static const char *const expect[5] = {"1B", "DB", "B4", "B8", "5F"};
    tad_asker_config_default(&cfg, 102, "D102");
    CHECK_EQ(tad_asker_init(&a, &cfg, NULL), XMSG_OK);
    a.state = TAD_ASKER_RUNNING;

    /* Default (strategy 1): a line goes whole up to its CR; ESC is a control character. */
    a.rfi_open = true;
    CHECK_EQ(tad_asker_write(&a, (const uint8_t *)"SYSTEM\r", 7, 0), 7);
    a.line_len = 0;
    a.rfi_open = true;
    CHECK_EQ(tad_asker_write(&a, slutt, 5, 0), 1);
    CHECK_BYTES(a.line, a.line_len, "1B");
    a.line_len = 0;
    a.rfi_open = true;
    CHECK_EQ(tad_asker_write(&a, slutt + 1, 4, 0), 4);
    CHECK_BYTES(a.line, a.line_len, "DBB4B85F");

    /* S3-CONFIG: strategy 0, one byte per grant, parity on. */
    a.line_len = 0;
    give_chain(&a, "000403000000000301FF0200");
    CHECK_EQ(a.break_strategy, 0);
    CHECK(a.rfi_open);
    for (size_t i = 0; i < 5; i++)
    {
        a.line_len = 0;
        a.rfi_open = true;
        CHECK_EQ(tad_asker_write(&a, slutt + i, 5 - i, 0), 1);
        CHECK_BYTES(a.line, a.line_len, expect[i]);
    }

    /* Back to strategy 1 (D100 at the exit, frame 1033: 00 04 03 01 00 00). */
    give_chain(&a, "0004030100000200");
    CHECK_EQ(a.break_strategy, 1);

    /* S3-CONFIG's screen letter, XMSGGW pane A 13:16:00.271 and hub frame 989: BMMX strategy 7,
     * BRKMAX 1, a table of all ones, then ECKM strategy 7 with its table, then RFI. */
    give_chain(&a, "000403010000"
                   "0413070001FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"
                   "00031107FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF0200");
    CHECK_EQ(a.break_strategy, 7);
    CHECK_EQ(a.break_max, 1);
    CHECK(a.break_table == TAD_BREAK_TABLE_ALL);
    CHECK_EQ(a.stats.unknown_break_strategy, 0);
    a.line_len = 0;
    a.rfi_open = true;
    CHECK_EQ(tad_asker_write(&a, slutt, 5, 0), 1);
    CHECK_BYTES(a.line, a.line_len, "1B");

    /* BRKMAX alone limits a strategy 3 and up even with no break character. */
    give_chain(&a, "04130700030000000000000000000000000000000000");
    CHECK(a.break_table == TAD_BREAK_TABLE_NONE);
    a.line_len = 0;
    a.rfi_open = true;
    CHECK_EQ(tad_asker_write(&a, slutt, 5, 0), 3);
    give_chain(&a, "0004030100000200");

    /* A strategy this file does not implement is counted and logged, and input is not cut. */
    give_chain(&a, "0004030300000200");
    CHECK_EQ(a.stats.unknown_break_strategy, 1);
    a.line_len = 0;
    a.rfi_open = true;
    CHECK_EQ(tad_asker_write(&a, slutt, 5, 0), 5);
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

    break_setting_rules();

    cfg.system_name = "";
    CHECK_EQ(tad_asker_init(&a, &cfg, NULL), XMSG_EINVAL);
    cfg.system_name = "A-NAME-FAR-TOO-LONG";
    CHECK_EQ(tad_asker_init(&a, &cfg, NULL), XMSG_EINVAL);
}

/* ------------------------------------------------------------------ logout, escape, type */

static unsigned s_logged_out;

static void w_logged_out(void *ctx)
{
    (void)ctx;
    s_logged_out++;
}

/* The host frame with this Flags1 in the capture. */
static const char *host_hex(uint16_t flags1)
{
    for (size_t i = 0; i < N_HOST; i++)
    {
        if (k_tad_host_frames[i].flags1 == flags1)
        {
            return k_tad_host_frames[i].hex;
        }
    }
    CHECK(false);
    return k_tad_host_frames[0].hex;
}

/* Bring a session to the terminal setup with the captured accept, port assignment and DUMM:
 * the asker has then sent letter, session setup, terminal setup and ESCA (4 frames). */
static uint32_t to_terminal_setup(const TadAskerConfig *cfg)
{
    TadAskerCallbacks cb = {&s_watch, w_text, w_state, w_gate, w_echo, NULL, w_logged_out};
    uint32_t now = bring_up(0x00F8);
    s_logged_out = 0;
    CHECK_EQ(tad_asker_init(&s_asker, cfg, &cb), XMSG_OK);
    CHECK_EQ(tad_asker_connect(&s_asker, &s_a, now), XMSG_OK);
    for (size_t k = 0; k < 3; k++)
    {
        for (int i = 0; i < 300 && s_host.count < k + 1u; i++)
        {
            pump(++now);
        }
        host_send_hex(k_tad_host_frames[k].hex, k_tad_host_frames[k].flags1, 0, now);
    }
    for (int i = 0; i < 400 && s_host.count < 4; i++)
    {
        pump(++now);
    }
    for (int i = 0; i < 100; i++)
    {
        pump(++now);
    }
    CHECK_EQ(s_asker.state, TAD_ASKER_TERMINAL_SETUP);
    CHECK_EQ(s_host.count, 4);
    return now;
}

/* SYCN 00 0B is the logout: raised once per chain, not again for a repeat of the frame, not for
 * other SYCN values and not for the 0xFD notice alone (record 595 of the capture file has an FD
 * right after the login). The asker does not hang up on its own; the reply rule is unchanged
 * (a DUMM, as for any content). */
static void logout_signal_once(void)
{
    TadAskerConfig cfg;
    char hex[256];
    tad_asker_config_default(&cfg, 102, "D102");
    uint32_t now = to_terminal_setup(&cfg);

    host_send_hex(host_hex(0x0136), 0x0136, 0, now);                  /* SYCN 0003 */
    host_send_hex(k_tad_host_frames[N_HOST - 1].hex, 0x0137, 0, now); /* FD alone */
    for (int i = 0; i < 200; i++)
    {
        pump(++now);
    }
    CHECK_EQ(s_logged_out, 0);
    CHECK_EQ(s_asker.stats.logouts, 0);
    CHECK_EQ(s_asker.stats.host_notices, 1);

    /* The same chain with SYCN 000A (logged in): not a logout. */
    snprintf(hex, sizeof hex, "%s", host_hex(0x0145));
    hex[strlen(hex) - 1u] = 'A';
    host_send_hex(hex, 0x0138, 0, now);
    for (int i = 0; i < 200; i++)
    {
        pump(++now);
    }
    CHECK_EQ(s_logged_out, 0);

    size_t before = s_host.count;
    host_send_hex(host_hex(0x0145), 0x0139, 0, now); /* "--EXIT--" + SYCN 000B */
    for (int i = 0; i < 200; i++)
    {
        pump(++now);
    }
    CHECK_EQ(s_logged_out, 1);
    CHECK_EQ(s_asker.stats.logouts, 1);
    CHECK_EQ(s_asker.state, TAD_ASKER_TERMINAL_SETUP); /* no hang-up by itself */
    CHECK_EQ(s_host.count, before + 1u);               /* the DUMM, as before */
    if (s_host.count == before + 1u)
    {
        CHECK_BYTES(s_host.frames[before] + 28, s_host.lens[before] - 28, "000000021800");
    }
    host_send_hex(host_hex(0x0145), 0x0139, 0, now); /* a repeat of the same frame */
    for (int i = 0; i < 200; i++)
    {
        pump(++now);
    }
    CHECK_EQ(s_logged_out, 1);
    CHECK(s_asker.stats.repeats_in >= 1);
}

/* tad_asker_escape sends the same bare ESCA frame as the terminal setup does, works with no
 * input grant open, and leaves typed text alone. Before the terminal setup it is refused. The
 * terminal type from the config lands in the TTYP bytes. */
static void escape_and_terminal_type(void)
{
    TadAskerConfig cfg;
    TadAsker idle;
    tad_asker_config_default(&cfg, 102, "D102");
    cfg.ttyp[0] = 0x00; /* type 93 decimal = 0x005D, high byte first */
    cfg.ttyp[1] = 0x5D;

    CHECK_EQ(tad_asker_init(&idle, &cfg, NULL), XMSG_OK);
    CHECK_EQ(tad_asker_escape(&idle, 0), XMSG_ESTATE); /* no session */
    CHECK_EQ(tad_asker_escape(NULL, 0), XMSG_EINVAL);
    idle.state = TAD_ASKER_PORT_ASSIGNED;
    idle.session_port = 0x04C2;
    CHECK_EQ(tad_asker_escape(&idle, 0), XMSG_ESTATE); /* would overtake the terminal setup */

    uint32_t now = to_terminal_setup(&cfg);
    /* Frame 2 is the terminal setup: TMOD 08, TTYP 00 5D, DESC 1B, OPSV 4C 01 04. */
    CHECK_BYTES(s_host.frames[2] + 28, s_host.lens[2] - 28,
                "0000000F0C01080D02005D0F011B1F034C0104");
    CHECK(!s_asker.rfi_open);
    CHECK_EQ(tad_asker_escape(&s_asker, now), XMSG_OK);
    for (int i = 0; i < 200 && s_host.count < 5; i++)
    {
        pump(++now);
    }
    CHECK_EQ(s_host.count, 5);
    CHECK_EQ(s_asker.stats.escapes, 1);
    if (s_host.count == 5)
    {
        /* Byte for byte the setup ESCA (frame 3) from the sub-header on: flags 0x82, role
         * 0x94, to the session port, XMCSM 0008, body 00 00 00 02 08 00. */
        CHECK_EQ(s_host.lens[4], s_host.lens[3]);
        CHECK(memcmp(s_host.frames[4] + 14, s_host.frames[3] + 14, s_host.lens[3] - 14) == 0);
        CHECK_BYTES(s_host.frames[4] + 14, 4, "21008294");
        CHECK_EQ(xmsg_be16_read(s_host.frames[4] + OFF_DST_PORT), 0x04C2);
        CHECK_BYTES(s_host.frames[4] + 26, s_host.lens[4] - 26, "0008000000020800");
    }

    /* Running, no grant, typed text waiting: ESCA still goes, and the asker's own pending text
     * is not touched. */
    s_asker.state = TAD_ASKER_RUNNING;
    s_asker.line_len = 3;
    CHECK_EQ(tad_asker_escape(&s_asker, now), XMSG_OK);
    CHECK_EQ(s_asker.line_len, 3);
    s_asker.line_len = 0;
    CHECK_EQ(tad_asker_hangup(&s_asker, now), XMSG_OK);
    CHECK_EQ(tad_asker_escape(&s_asker, now), XMSG_ESTATE); /* closing */
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
    letter_to_a_system_beyond_the_neighbour_names_the_target();
    refused_letter_fails_with_the_xrout_code();
    silent_host_times_out();
    xense_resends_and_other_errors_fail();
    letter_is_sent_again_after_a_host_restart();
    hangup_before_the_port_goes_out_with_the_port();
    hangup_while_running_drops_what_waits();
    who_are_you_is_answered_like_the_real_asker();
    who_are_you_default_name_and_limits();
    unanswered_letter_draws_one_resync();
    logout_signal_once();
    escape_and_terminal_type();
    TEST_DONE();
}
