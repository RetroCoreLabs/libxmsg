/*
 * test_cosmos_fa.c - libcosmos: QFORM, the password fold, and the FA client (read, write,
 * listing) replayed against a real SINTRAN file server's captured messages.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * The replays run two real nodes back to back on the in-memory pair. Node 100 carries our FA
 * client. Node 102 plays the D102 file server: it sends the CAPTURED server messages, each one
 * when our client has sent the message that came before it on the real machines, and it checks
 * that every message our client sends equals the one the real D100 client sent, byte for byte.
 * Two kinds of byte may differ, both named where they are allowed:
 *   - the pad byte after F2 00FF: the real client sends whatever was in its buffer, we send 0;
 *   - the character after the apostrophe in an open (BUGS.md C1): the real client sent "T" and
 *     "U" in these captures; we send ".", as RetroIO's live-proven reader does.
 * The captured LIST-FILES client also left bytes from an earlier use of its buffers behind the
 * first apostrophe of two fields: the remote user slot of the reserve ("SYS'EM'") and the name
 * block of each walk request. That replay compares those two fields up to the apostrophe only;
 * the FILE-STATISTICS capture has clean zeros there and is compared in full.
 */
#include "libcosmos/fa/fa_client.h"
#include "libcosmos/fa/fa_replies.h"
#include "libcosmos/fa/fa_requests.h"
#include "libcosmos/fa/fa_text.h"
#include "libcosmos/qform/qform.h"
#include "libcosmos/sintran/sintran_password.h"
#include "libxmsg/xmsg/xmsg_node.h"

#include "pair/test_pair.h"
#include "test_util.h"
#include "vectors/fa_list_d100_d102.h"
#include "vectors/fa_open_error_d100_d102.h"
#include "vectors/fa_read_d100_d102.h"
#include "vectors/fa_stat_d102_d100.h"
#include "vectors/fa_write_d100_d102.h"

#define N_READ  (sizeof k_fa_read_events / sizeof k_fa_read_events[0])
#define N_ERROR (sizeof k_fa_open_error_events / sizeof k_fa_open_error_events[0])
#define N_LIST  (sizeof k_fa_list_events / sizeof k_fa_list_events[0])
#define N_STAT  (sizeof k_fa_stat_events / sizeof k_fa_stat_events[0])
#define N_WRITE (sizeof k_fa_write_events / sizeof k_fa_write_events[0])

/* The file the captured write carried, WRTEST1:OUT. */
#define WRITE_LENGTH 17905u

/* The server's session port in the captures (the confirm and everything after come from it). */
#define SERVER_PORT 0x06B6u

/* ------------------------------------------------------------------ the password fold */

static void password_fold_vectors(void)
{
    /* "secret" -> 6D 2A: the captured reserve of a read as user SECRET (RetroIO
     * DOC/XMSG-FA-ACCESS-PASSWORD-ON-THE-WIRE-2026-07-29.md). Case does not matter. */
    CHECK_EQ(sintran_password_fold("secret"), 0x6D2A);
    CHECK_EQ(sintran_password_fold("SECRET"), 0x6D2A);
    /* RetroIO SintranPassword tests: ORANGE = 14378, and digits go in raw ("12"). */
    CHECK_EQ(sintran_password_fold("ORANGE"), 0x382A);
    CHECK_EQ(sintran_password_fold("12"), 0x01BA);
    CHECK_EQ(sintran_password_fold(""), 0);
    CHECK_EQ(sintran_password_fold(NULL), 0);
}

/* ------------------------------------------------------------------ QFORM */

static void qform_walks_into_constructed_values(void)
{
    uint8_t buf[128];
    QformReader r;
    QformItem it;
    uint32_t v;
    /* The fields of the captured reserve (02:29:11.129) after op and seq. */
    int len = test_hex_to_bytes("F20001A207D0F200028C06920001920001F20003BD42414B3034202053595354"
                                "454DF200FF",
                                buf, sizeof buf);
    static const uint8_t tags[] = {0xF2, 0xA2, 0xF2, 0x8C, 0x92, 0x92, 0xF2, 0xBD, 0xF2};
    static const uint8_t depth[] = {0, 0, 0, 0, 1, 1, 0, 0, 0};
    size_t got = 0;

    qform_reader_init(&r, buf, (size_t)len);
    while (qform_reader_next(&r, &it))
    {
        CHECK(got < sizeof tags);
        if (got < sizeof tags)
        {
            CHECK_EQ(it.tag, tags[got]);
            CHECK_EQ(it.depth, depth[got]);
        }
        got++;
    }
    CHECK_EQ(got, sizeof tags);
    CHECK(!r.failed);
    CHECK(qform_find_field(buf, (size_t)len, 3, &it));
    CHECK_EQ(it.len, 13);
    CHECK(memcmp(it.value, "BAK04  SYSTEM", 13) == 0);
    CHECK(qform_find_field(buf, (size_t)len, 1, &it));
    CHECK(qform_item_uint(&it, &v));
    CHECK_EQ(v, 0x07D0);
    CHECK(!qform_find_field(buf, (size_t)len, 9, &it));

    /* The escape marker: 8C 80 05 is a five-byte constructed value (SetEndOfFile shape). */
    len = test_hex_to_bytes("F200028C8005A400000123F200FF", buf, sizeof buf);
    CHECK(qform_find_field(buf, (size_t)len, 2, &it));
    CHECK_EQ(it.cls, QFORM_CLASS_CONSTRUCTED);
    CHECK_EQ(it.len, 5);

    /* A lone last byte is padding even with bit 7 set (SINTRAN pads with F0). */
    len = test_hex_to_bytes("920008920005F200FFF0", buf, sizeof buf);
    qform_reader_init(&r, buf, (size_t)len);
    got = 0;
    while (qform_reader_next(&r, &it))
    {
        got++;
    }
    CHECK_EQ(got, 3); /* 92, 92, F2 00FF; the F0 is not a tag */
    CHECK(!r.failed);

    /* A value that runs past the end is malformed. */
    len = test_hex_to_bytes("B0100102", buf, sizeof buf);
    qform_reader_init(&r, buf, (size_t)len);
    CHECK(!qform_reader_next(&r, &it));
    CHECK(r.failed);
}

static void qform_writer_forms(void)
{
    uint8_t buf[64];
    uint8_t big[20];
    QformWriter w;
    size_t at;

    qform_writer_init(&w, buf, sizeof buf);
    qform_put_selector(&w, 2);
    at = qform_begin_constructed(&w);
    qform_put_tag16(&w, QFORM_TAG_INT16, 1);
    qform_put_tag16(&w, QFORM_TAG_INT16, 1);
    qform_end_constructed(&w, at);
    qform_put_typed32(&w, 5);
    CHECK(!w.failed);
    CHECK_BYTES(buf, w.len, "F20002 8C06 920001 920001 A400000005");

    memset(big, 'A', sizeof big);
    qform_writer_init(&w, buf, sizeof buf);
    qform_put_string(&w, big, 15);
    CHECK_EQ(buf[0], 0xBF);
    qform_writer_init(&w, buf, sizeof buf);
    qform_put_string(&w, big, 16);
    CHECK_EQ(buf[0], 0xB0);
    CHECK_EQ(buf[1], 0x10);
    qform_writer_init(&w, buf, sizeof buf);
    qform_put_string(&w, big, 0);
    CHECK(w.failed);
    qform_writer_init(&w, buf, 3);
    qform_put_typed32(&w, 1);
    CHECK(w.failed);
}

/* ------------------------------------------------------------------ builders */

static void replies_are_read(void)
{
    uint8_t buf[96];
    FaReply r;
    uint16_t fno;
    uint32_t flen;
    /* Captured open reply (02:29:12.489): file 0x40, 0x45F1 = 17905 bytes. */
    int len = test_hex_to_bytes("07F00002810090BB920005920002F20002A20040F20003A4000045F1F200FF00",
                                buf, sizeof buf);
    CHECK(fa_reply_parse(buf, (size_t)len, &r));
    CHECK_EQ(r.op, FA_OP_OPEN);
    CHECK_EQ(r.seq, 2);
    CHECK(!r.refused);
    CHECK(fa_reply_open_info(&r, &fno, &flen));
    CHECK_EQ(fno, 0x40);
    CHECK_EQ(flen, 17905);
    /* D100 open reply for the 0-byte (SYSTEM)RONNY-R:TXT (XMSGGW dump 2026-11-12 14:40:03.501,
     * pad byte 52 as sent): no selector 3, which means 0 bytes. */
    len = test_hex_to_bytes("07F0000281009112920005920002F20002A20040F200FF52", buf, sizeof buf);
    CHECK(fa_reply_parse(buf, (size_t)len, &r));
    CHECK(!r.refused);
    flen = 99;
    CHECK(fa_reply_open_info(&r, &fno, &flen));
    CHECK_EQ(fno, 0x40);
    CHECK_EQ(flen, 0);
    /* Captured refusal (capture-open-error.txt 02:37:42.349): error 46 = 0x2E. */
    len = test_hex_to_bytes("07F00002810090BB920005920002F20001A2002EF200FFA4", buf, sizeof buf);
    CHECK(fa_reply_parse(buf, (size_t)len, &r));
    CHECK(r.refused);
    CHECK_EQ(r.error, FA_ERR_NO_SUCH_FILE);
    CHECK(strcmp(fa_error_name(r.error), "no such file") == 0);
    CHECK(!fa_reply_parse(buf, 10, &r));
}

static void builders_refuse_what_does_not_fit(void)
{
    uint8_t buf[FA_REQUEST_MAX];
    FaEnvelope e = {FA_TYPE_REQUEST, 0x42, 0x80, FA_TOKEN_CLIENT_FIRST};

    CHECK_EQ(fa_build_reserve(buf, sizeof buf, &e, 1, "BAK04", "SYSTEM", "SYSTEM", 0), 112);
    /* The asker field holds 15 bytes: "BAK04  " + 9 is too long. */
    CHECK_EQ(fa_build_reserve(buf, sizeof buf, &e, 1, "BAK04", "LONGUSERS", "SYSTEM", 0), 0);
    /* The remote user needs its apostrophe inside 16: 15 letters fit. "BAK04  A" is 5 bytes
     * shorter than "BAK04  SYSTEM", so 107, padded to 108. */
    CHECK_EQ(fa_build_reserve(buf, sizeof buf, &e, 1, "BAK04", "A", "SIXTEEN-LETTERS", 0), 108);
    CHECK_EQ(fa_build_reserve(buf, sizeof buf, &e, 1, "BAK04", "A", "SIXTEEN-LETTERSX", 0), 0);
    CHECK_EQ(fa_build_reserve(buf, 100, &e, 1, "BAK04", "SYSTEM", "SYSTEM", 0), 0);
    /* An odd system name in the letter would need a fill byte no capture shows. */
    CHECK_EQ(fa_build_connect_letter(buf, sizeof buf, "D99", 2), 0);
    CHECK_EQ(fa_build_connect_letter(buf, sizeof buf, "D102", 2), 34);
    /* The password word lands big-endian in its slot. */
    CHECK_EQ(fa_build_reserve(buf, sizeof buf, &e, 1, "BAK03", "SYSTEM", "SECRET",
                              sintran_password_fold("secret")),
             112);
    CHECK_EQ(buf[75], 0x6D);
    CHECK_EQ(buf[76], 0x2A);
}

/* ------------------------------------------------------------------ two nodes */

#define MAX_SEEN 16

typedef struct Server
{
    uint8_t bodies[MAX_SEEN][FA_DATA_MESSAGE_SIZE];
    size_t lens[MAX_SEEN];
    XmsgSubhdr subs[MAX_SEEN];
    size_t head;
    size_t count;
} Server;

/* What one replay asks the client to do, as the captured client did it. */
typedef struct Run
{
    bool listing;
    const char *remote_user;
    const char *asker;
    uint16_t peer_system;
    bool mask_residue; /* see the file comment */
    bool writing;      /* fa_client_write of s_write_file */
} Run;

static Run s_run;

typedef struct Sink
{
    uint8_t file[32768];
    uint32_t got; /* bytes in order */
    bool out_of_order;
    uint32_t opened_length;
    int opened_calls;
    int done_calls;
    int result;
    uint16_t sintran_error;
    FaFileEntry entries[64];
    size_t entry_count;
    char directory[17];
    char user[17];
    int directory_calls;
    size_t cancel_at_entry; /* nonzero: fa_client_cancel from inside that entry callback */
    bool cancel_at_data;    /* fa_client_cancel from inside the first data callback */
    bool cancelled;         /* fa_client_cancel has been called */
    int data_calls;
    uint32_t cancel_at_offset; /* nonzero: fa_client_cancel when the source is asked for it */
    uint32_t fail_at_offset;   /* nonzero: the source gives nothing for that offset */
    int source_calls;
} Sink;

static XmsgNode s_a; /* our client's machine, node 100 */
static XmsgNode s_b; /* the file server, node 102 */
static TestPair s_pair;
static TestPairPort s_port_a;
static TestPairPort s_port_b;
static Server s_server;
static Sink s_sink;
static FaClient s_client;
static uint8_t s_write_file[WRITE_LENGTH]; /* the captured write's content, see write_file_load */

static void server_keep(void *ctx, const XmsgDatagram *dg)
{
    Server *sv = (Server *)ctx;
    size_t slot;

    if (sv->count >= MAX_SEEN || dg->body_len > sizeof sv->bodies[0])
    {
        CHECK(false);
        return;
    }
    slot = (sv->head + sv->count) % MAX_SEEN;
    memcpy(sv->bodies[slot], dg->body, dg->body_len);
    sv->lens[slot] = dg->body_len;
    sv->subs[slot] = dg->sub;
    sv->count++;
}

static void server_on_letter(void *ctx, const XmsgDatagram *dg, uint32_t now_ms)
{
    (void)now_ms;
    server_keep(ctx, dg);
}

static void sink_opened(void *ctx, uint32_t length)
{
    Sink *k = (Sink *)ctx;
    k->opened_length = length;
    k->opened_calls++;
}

static void sink_data(void *ctx, uint32_t offset, const uint8_t *bytes, size_t n)
{
    Sink *k = (Sink *)ctx;
    if (offset != k->got || offset + n > sizeof k->file)
    {
        k->out_of_order = true;
        return;
    }
    memcpy(k->file + offset, bytes, n);
    k->got += (uint32_t)n;
    k->data_calls++;
    if (k->cancel_at_data && !k->cancelled)
    {
        k->cancelled = true;
        CHECK_EQ(fa_client_cancel(&s_client, 0), XMSG_OK);
    }
}

static void sink_done(void *ctx, int result, uint16_t sintran_error)
{
    Sink *k = (Sink *)ctx;
    k->done_calls++;
    k->result = result;
    k->sintran_error = sintran_error;
}

static size_t sink_source(void *ctx, uint32_t offset, uint8_t *out, size_t n)
{
    Sink *k = (Sink *)ctx;
    k->source_calls++;
    if (k->fail_at_offset != 0u && offset == k->fail_at_offset)
    {
        return 0;
    }
    if (k->cancel_at_offset != 0u && offset == k->cancel_at_offset && !k->cancelled)
    {
        k->cancelled = true;
        CHECK_EQ(fa_client_cancel(&s_client, 0), XMSG_OK);
    }
    CHECK((size_t)offset + n <= sizeof s_write_file);
    memcpy(out, s_write_file + offset, n);
    return n;
}

static void sink_directory(void *ctx, const char *directory, const char *user)
{
    Sink *k = (Sink *)ctx;
    k->directory_calls++;
    snprintf(k->directory, sizeof k->directory, "%s", directory);
    snprintf(k->user, sizeof k->user, "%s", user);
}

static void sink_entry(void *ctx, const FaFileEntry *e)
{
    Sink *k = (Sink *)ctx;
    if (k->entry_count < sizeof k->entries / sizeof k->entries[0])
    {
        k->entries[k->entry_count] = *e;
    }
    k->entry_count++;
    if (k->cancel_at_entry != 0u && k->entry_count == k->cancel_at_entry && !k->cancelled)
    {
        k->cancelled = true;
        CHECK_EQ(fa_client_cancel(&s_client, 0), XMSG_OK);
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

static uint32_t bring_up(void)
{
    XmsgTransportPort ta;
    XmsgTransportPort tb;
    XmsgNodeConfig cfg;
    XmsgPorts ports;
    XmsgNodeCallbacks cb;
    uint32_t now = 0;

    memset(&s_server, 0, sizeof s_server);
    memset(&s_sink, 0, sizeof s_sink);
    test_pair_init(&s_pair);
    test_pair_transport(&s_pair, 0, &s_port_a, &ta);
    test_pair_transport(&s_pair, 1, &s_port_b, &tb);

    xmsg_node_config_default(&cfg, 100);
    cfg.lapb.t1_ms = 50;
    cfg.settle_ms = 20;
    memset(&ports, 0, sizeof ports);
    ports.transport = ta;
    memset(&cb, 0, sizeof cb);
    CHECK_EQ(xmsg_node_init(&s_a, &cfg, &ports, &cb), XMSG_OK);

    xmsg_node_config_default(&cfg, 102);
    cfg.own_system = s_run.peer_system; /* 0: system 102; the FILE-STATISTICS capture has D100 */
    cfg.lapb.t1_ms = 50;
    cfg.settle_ms = 20;
    cfg.reject_closed_ports = false; /* a tap: every datagram comes up unhandled */
    memset(&ports, 0, sizeof ports);
    ports.transport = tb;
    memset(&cb, 0, sizeof cb);
    cb.ctx = &s_server;
    cb.on_letter = server_on_letter;
    cb.unhandled = server_keep;
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

static void client_start(const FaClientConfig *cfg, const char *spec, uint32_t now)
{
    FaClientCallbacks cb = {&s_sink,        sink_opened, sink_data,  sink_done,
                            sink_directory, sink_entry,  sink_source};
    const char *remote = (s_run.remote_user != NULL) ? s_run.remote_user : "SYSTEM";
    uint16_t system = (s_run.peer_system != 0u) ? s_run.peer_system : 102u;
    CHECK_EQ(fa_client_init(&s_client, cfg, &cb), XMSG_OK);
    if (s_run.listing)
    {
        CHECK_EQ(fa_client_list(&s_client, &s_a, 102, system, "SYSTEM", remote, "", spec, now),
                 XMSG_OK);
    }
    else if (s_run.writing)
    {
        CHECK_EQ(fa_client_write(&s_client, &s_a, 102, system, "SYSTEM", remote, "", spec,
                                 WRITE_LENGTH, FA_WRITE_CREATE, now),
                 XMSG_OK);
    }
    else
    {
        CHECK_EQ(fa_client_read(&s_client, &s_a, 102, system, "SYSTEM", remote, "", spec, now),
                 XMSG_OK);
    }
}

/* Send one server message from node 102 to our client's port. XMSG_EAGAIN when the window is
 * full (a data message takes two frames). */
static int server_send(const uint8_t *body, size_t len, uint32_t now)
{
    XmsgSubhdr sub;
    uint8_t *buf;
    size_t cap;
    int rc = xmsg_node_send_begin(&s_b, &buf, &cap);
    if (rc != XMSG_OK)
    {
        return rc;
    }
    memcpy(buf, body, len);
    memset(&sub, 0, sizeof sub);
    sub.frame_flags = XMSG_FF_CONTROL_BARE; /* as captured: 82/84 */
    sub.role = FA_ROLE_DATA;
    sub.dst_system = 100;
    sub.dst_port = s_client.base.local_port;
    sub.src_port = SERVER_PORT;
    sub.xmcsm = (uint16_t)len;
    rc = xmsg_node_send_commit(&s_b, 100, &sub, len, now, NULL);
    if (rc != XMSG_OK)
    {
        xmsg_node_send_cancel(&s_b);
    }
    return rc;
}

/* Index of the F2 00 FF that ends a request's field list, or 0. */
static size_t end_of_fields(const uint8_t *b, size_t n)
{
    for (size_t i = n; i >= 3; i--)
    {
        if (b[i - 3] == 0xF2 && b[i - 2] == 0x00 && b[i - 1] == 0xFF)
        {
            return i - 3;
        }
    }
    return 0;
}

/* Compare what our client sent with what the real client sent (see the file comment). */
static void check_client_message(const uint8_t *ours, size_t ours_len, const XmsgSubhdr *sub,
                                 const char *captured_hex)
{
    /* Zeroed: only the first `len` bytes are decoded, so nothing past them may hold stack data. */
    uint8_t want[FA_DATA_MESSAGE_SIZE] = {0};
    uint8_t got[FA_DATA_MESSAGE_SIZE] = {0};
    int len = test_hex_to_bytes(captured_hex, want, sizeof want);
    size_t end;

    CHECK(len >= 0); /* -1: the captured hex did not decode or does not fit */
    CHECK_EQ(ours_len, len);
    /* Both bounds before the copy: a bad capture string gives len -1, which as a size_t would
     * pass the length compare for a huge ours_len (gcc 15 -O3 -Wstringop-overflow found it). */
    if (len < 0 || ours_len != (size_t)len || ours_len > sizeof got)
    {
        CHECK_BYTES(ours, ours_len, captured_hex);
        return;
    }
    memcpy(got, ours, ours_len);
    end = end_of_fields(want, (size_t)len);
    /* A request is at least the 8-byte envelope and "92 op": bytes 9 and 10 are read below
     * (found by MinGW gcc -O3 -Wmaybe-uninitialized: a shorter capture would read past it). */
    if (len >= 11 && want[0] == 0x07 && want[1] == 0xF0 && end > 0)
    {
        if (end + 3u == (size_t)len - 1u)
        {
            CHECK_EQ(got[len - 1], 0x00); /* our pad */
            got[len - 1] = want[len - 1];
        }
        if (want[9] == 0x00 && want[10] == FA_OP_OPEN && !s_run.writing)
        {
            CHECK_EQ(got[end - 1], '.'); /* BUGS.md C1 */
            got[end - 1] = want[end - 1];
        }
    }
    if (s_run.mask_residue && want[0] == 0x07 && want[1] == 0xF0 && ours_len > 90u)
    {
        /* The reserve's remote user slot is bytes 55..70; a walk's name block is 28..89. */
        size_t from = (want[9] == 0x00 && want[10] == FA_OP_RESERVE) ? 55u : 28u;
        size_t to = (from == 55u) ? 71u : 90u;
        bool past = false;
        for (size_t i = from; i < to; i++)
        {
            if (past)
            {
                got[i] = want[i];
            }
            past = past || (want[i] == 0x27);
        }
    }
    CHECK_BYTES(got, ours_len, captured_hex);
    /* The sub-header as captured: the letter 86/E4 to port 0, the rest 82/84; XMCSM = length. */
    if (want[0] == 0x1B)
    {
        CHECK_EQ(sub->frame_flags, XMSG_FF_SETUP);
        CHECK_EQ(sub->role, FA_ROLE_LETTER);
        CHECK_EQ(sub->dst_port, 0);
    }
    else
    {
        CHECK_EQ(sub->frame_flags, XMSG_FF_CONTROL_BARE);
        CHECK_EQ(sub->role, FA_ROLE_DATA);
        CHECK_EQ(sub->dst_port, SERVER_PORT);
    }
    CHECK_EQ(sub->dst_system, (s_run.peer_system != 0u) ? s_run.peer_system : 102u);
    CHECK_EQ(sub->xmcsm, ours_len);
}

/* The first client event at or after i, or n. */
static size_t next_client_event(const FaVectorEvent *ev, size_t n, size_t i)
{
    while (i < n && ev[i].who != 'C')
    {
        i++;
    }
    return i;
}

/* Play a captured conversation: our client against the captured server. A server message is
 * sent once every client message before it in the capture has arrived; the client's messages
 * must come in the captured order. How the two streams interleave in time is not part of the
 * protocol (a short ack can overtake a data message on the real wire too), so it is not
 * checked. Returns true when every event was used. `dup_confirm` sends the confirm twice. */
static bool replay(const FaVectorEvent *ev, size_t n, const char *spec, bool dup_confirm)
{
    FaClientConfig cfg;
    uint32_t now = bring_up();
    uint32_t until = now + 60000u;
    size_t c_idx = next_client_event(ev, n, 0);
    size_t s_idx = 0;
    bool dup_sent = false;
    uint8_t body[FA_DATA_MESSAGE_SIZE];

    fa_client_config_default(&cfg);
    if (s_run.asker != NULL)
    {
        snprintf(cfg.asker, sizeof cfg.asker, "%s", s_run.asker);
    }
    client_start(&cfg, spec, now);
    while (now < until && s_sink.done_calls == 0)
    {
        now++;
        pump(now);
        while (s_server.count > 0)
        {
            size_t slot = s_server.head;
            CHECK(c_idx < n);
            if (c_idx < n)
            {
                check_client_message(s_server.bodies[slot], s_server.lens[slot],
                                     &s_server.subs[slot], ev[c_idx].hex);
                c_idx = next_client_event(ev, n, c_idx + 1u);
            }
            s_server.head = (s_server.head + 1u) % MAX_SEEN;
            s_server.count--;
        }
        while (s_idx < n)
        {
            int len;
            if (ev[s_idx].who == 'C')
            {
                if (s_idx >= c_idx)
                {
                    break; /* that client message has not come yet */
                }
                s_idx++;
                continue;
            }
            len = test_hex_to_bytes(ev[s_idx].hex, body, sizeof body);
            if (server_send(body, (size_t)len, now) != XMSG_OK)
            {
                break; /* window full: next millisecond */
            }
            if (dup_confirm && !dup_sent && body[0] == 0x07 && body[1] == 0xD2)
            {
                dup_sent = true;
                continue; /* the same confirm again on the next pass */
            }
            s_idx++;
        }
    }
    /* Let the last acknowledgements settle. */
    for (uint32_t i = 0; i < 200u; i++)
    {
        now++;
        pump(now);
    }
    CHECK_EQ(c_idx, n);
    CHECK_EQ(s_idx, n);
    return c_idx == n && s_idx == n;
}

static void read_replays_the_capture(void)
{
    uint8_t body[FA_DATA_MESSAGE_SIZE];
    uint8_t want[32768];
    uint32_t want_len = 0;
    CHECK(replay(k_fa_read_events, N_READ, "PATCH-FILE:OUT", false));
    CHECK_EQ(s_sink.done_calls, 1);
    CHECK_EQ(s_sink.result, 0);
    CHECK_EQ(s_sink.sintran_error, 0);
    CHECK_EQ(fa_client_state(&s_client), FA_CLIENT_DONE);
    CHECK_EQ(s_sink.opened_calls, 1);
    CHECK_EQ(s_sink.opened_length, 17905);
    CHECK_EQ(s_client.blocks, 9);
    CHECK(s_client.has_object_entry);
    CHECK(!s_sink.out_of_order);
    /* The file is the 1024-byte payloads of the captured data messages, in order, cut at the
     * length the open reply gave. */
    for (size_t i = 0; i < N_READ; i++)
    {
        int len = test_hex_to_bytes(k_fa_read_events[i].hex, body, sizeof body);
        if (k_fa_read_events[i].who == 'S' && len == (int)FA_DATA_MESSAGE_SIZE)
        {
            memcpy(want + want_len, body + FA_ENVELOPE_SIZE, FA_DATA_BYTES);
            want_len += FA_DATA_BYTES;
        }
    }
    CHECK_EQ(want_len, 18 * FA_DATA_BYTES);
    CHECK_EQ(s_sink.got, 17905);
    CHECK(memcmp(s_sink.file, want, 17905) == 0);
    /* The session is gone from the node once the server closed. */
    CHECK(!s_client.base.attached);
    CHECK_EQ(s_client.stats.ignored, 0);
}

static void refused_open_still_gives_the_seat_back(void)
{
    CHECK(replay(k_fa_open_error_events, N_ERROR, "NO-SUCH-FILE:OUT", false));
    CHECK_EQ(s_sink.done_calls, 1);
    CHECK_EQ(s_sink.result, FA_CLIENT_EREFUSED);
    CHECK_EQ(s_sink.sintran_error, FA_ERR_NO_SUCH_FILE);
    CHECK_EQ(s_sink.opened_calls, 0);
    CHECK_EQ(s_sink.got, 0);
    CHECK_EQ(fa_client_state(&s_client), FA_CLIENT_DONE);
}

static void repeated_confirm_changes_nothing(void)
{
    CHECK(replay(k_fa_read_events, N_READ, "PATCH-FILE:OUT", true));
    CHECK_EQ(s_sink.result, 0);
    CHECK_EQ(s_sink.got, 17905);
    CHECK_EQ(s_client.stats.ignored, 1);
}

static void listing_replays_the_capture(void)
{
    FaNdDate d;
    s_run = (Run){true, "SYS", "BAK04", 0, true, false};
    CHECK(replay(k_fa_list_events, N_LIST, "", false));
    s_run = (Run){false, NULL, NULL, 0, false, false};

    CHECK_EQ(s_sink.done_calls, 1);
    CHECK_EQ(s_sink.result, 0); /* 197, the end of the directory, is the normal end */
    CHECK_EQ(s_sink.sintran_error, 0);
    CHECK_EQ(s_sink.entry_count, 49);
    CHECK_EQ(s_sink.directory_calls, 1);
    CHECK(strcmp(s_sink.directory, "PACK-ONE") == 0);
    CHECK(strcmp(s_sink.user, "SYSTEM") == 0);
    /* The first and the last file, as the captured records give them. */
    CHECK(strcmp(s_sink.entries[0].name, "SINTRAN") == 0);
    CHECK(strcmp(s_sink.entries[0].type, "DATA") == 0);
    CHECK_EQ(s_sink.entries[0].pages, 0x3F);
    CHECK_EQ(s_sink.entries[0].bytes, 0x1E000);
    CHECK_EQ(s_sink.entries[0].open_total, 10);
    CHECK(fa_nd_date_split(s_sink.entries[0].created, &d));
    CHECK(strcmp(s_sink.entries[48].name, "ENCOS-LOAD-0-B01") == 0);
    CHECK(strcmp(s_sink.entries[48].type, "LIST") == 0);
    /* A peripheral file: no byte count (FFFFFFFF on the wire). */
    CHECK(strcmp(s_sink.entries[3].name, "MAILBOX") == 0);
    CHECK(!s_sink.entries[3].has_bytes);
    CHECK_EQ(s_client.stats.ignored, 0);
    /* Every captured record has one version: next-version points at its own slot. */
    for (size_t i = 0; i < 49u; i++)
    {
        CHECK_EQ(fa_text_version(&s_sink.entries[i]), 1);
    }
    {
        char name[FA_TEXT_NAME];
        CHECK_EQ(fa_text_name(&s_sink.entries[0], FA_NAME_SINTRAN_VERSION, name, sizeof name), 14);
        CHECK(strcmp(name, "SINTRAN:DATA;1") == 0);
        CHECK_EQ(fa_text_name(&s_sink.entries[0], FA_NAME_FTP_VERSION, name, sizeof name), 14);
        CHECK(strcmp(name, "SINTRAN.DATA-1") == 0);
        CHECK_EQ(fa_text_name(&s_sink.entries[48], FA_NAME_FTP, name, sizeof name), 21);
        CHECK(strcmp(name, "ENCOS-LOAD-0-B01.LIST") == 0);
    }
}

static void file_statistics_replays_the_capture(void)
{
    s_run = (Run){true, "SYSTEM", "BAK05", 100, false, false};
    CHECK(replay(k_fa_stat_events, N_STAT, "SINTRAN:DATA", false));
    s_run = (Run){false, NULL, NULL, 0, false, false};

    CHECK_EQ(s_sink.result, 0);
    CHECK_EQ(s_sink.entry_count, 1);
    CHECK(strcmp(s_sink.entries[0].name, "SINTRAN") == 0);
    CHECK_EQ(s_sink.entries[0].bytes, 0x1E000);
    CHECK(strcmp(s_sink.directory, "PACK-ONE") == 0);
}

static void walk_requests_and_dates(void)
{
    uint8_t block[FA_WALK_BLOCK_SIZE];
    uint8_t want[FA_WALK_BLOCK_SIZE];
    FaNdDate d;

    CHECK(fa_build_walk_block(block, "SYSTEM", "SINTRAN:DATA"));
    memset(want, 0, sizeof want);
    memcpy(want, "(SYSTEM)SINTRAN:DATA'SINTRAN:DATA'", 34);
    CHECK(memcmp(block, want, sizeof want) == 0);
    CHECK(fa_build_walk_block(block, "SYSTEM", ""));
    CHECK(memcmp(block, "(SYSTEM)''", 10) == 0);
    CHECK_EQ(block[10], 0);
    /* "(" user ")" and two copies of the name must fit in 62. */
    CHECK(!fa_build_walk_block(block, "SYSTEM", "ABCDEFGHIJKLMNOPQRSTUVWXYZ0"));
    /* An ND date word: 0xC1FAAA75 is year 48 + 1950. */
    CHECK(fa_nd_date_split(0xC1FAAA75u, &d));
    CHECK_EQ(d.year, 1998);
    CHECK_EQ(d.month, 7);
    CHECK(!fa_nd_date_split(0, &d));
}

static void silent_server_times_out(void)
{
    FaClientConfig cfg;
    uint32_t now = bring_up();
    uint32_t until;

    fa_client_config_default(&cfg);
    cfg.reply_timeout_ms = 2000;
    client_start(&cfg, "PATCH-FILE:OUT", now);
    until = now + 5000u;
    while (now < until && s_sink.done_calls == 0)
    {
        now++;
        pump(now);
        s_server.count = 0; /* the server hears the letter and says nothing */
    }
    CHECK_EQ(s_sink.done_calls, 1);
    CHECK_EQ(s_sink.result, XMSG_ETIMEDOUT);
    CHECK_EQ(fa_client_state(&s_client), FA_CLIENT_FAILED);
    CHECK(!s_client.base.attached);
    /* A finished client can start again. */
    CHECK_EQ(fa_client_read(&s_client, &s_a, 102, 102, "SYSTEM", "SYSTEM", "", "X:Y", now),
             XMSG_OK);
    CHECK_EQ(fa_client_read(&s_client, &s_a, 102, 102, "SYSTEM", "SYSTEM", "", "X:Y", now),
             XMSG_ESTATE);
}

/* ------------------------------------------------------------------ cancel */

/* What the client sent after fa_client_cancel, as the test server saw it. */
typedef struct Teardown
{
    uint16_t ops[8]; /* the op of each request, in order */
    size_t op_count;
    int finished; /* session finished (0782) messages */
} Teardown;

/* When the test cancels. */
typedef struct CancelAt
{
    bool at_start;      /* right after the client started, before the confirm */
    size_t at_entry;    /* nonzero: inside the entry callback of that file */
    bool at_first_data; /* inside the first data callback */
    uint32_t at_offset; /* nonzero: inside the source call for that file offset (a write) */
} CancelAt;

static Teardown s_down;

/* The test server after the cancel: it answers each request as the captured server answered
 * a close and a release (capture-read.txt 02:29:16.269 to 02:29:16.778): a short ack, then a
 * reply with the request's op and sequence and no selector 1; and session finished with the
 * server's close. */
static void answer_teardown(const uint8_t *msg, size_t len, uint32_t now)
{
    static const uint8_t k_ack[] = {0x07, 0xA2, 0x00, 0x02, 0x0E, 0x00, 0x92, 0x2A};
    static const uint8_t k_close[] = {0x07, 0xC0, 0x00, 0x02, 0x00, 0x42, 0x00, 0x00};
    uint8_t reply[18] = {0x07, 0xF0, 0x00, 0x02, 0x9F, 0x00, 0x90, 0xBB, 0x92,
                         0x00, 0x00, 0x92, 0x00, 0x00, 0xF2, 0x00, 0xFF, 0x8C};
    uint16_t type;

    if (len < 8u)
    {
        return;
    }
    type = (uint16_t)((msg[0] << 8u) | msg[1]);
    if (type == FA_TYPE_SESSION_FINISHED)
    {
        s_down.finished++;
        CHECK_EQ(server_send(k_close, sizeof k_close, now), XMSG_OK);
        return;
    }
    if (type != FA_TYPE_REQUEST || len < 14u || len == FA_DATA_MESSAGE_SIZE || msg[8] != 0x92u)
    {
        return; /* a short ack, or the letter when cancelled before the confirm */
    }
    if (s_down.op_count < sizeof s_down.ops / sizeof s_down.ops[0])
    {
        s_down.ops[s_down.op_count] = (uint16_t)((msg[9] << 8u) | msg[10]);
    }
    s_down.op_count++;
    memcpy(reply + 9, msg + 9, 2);   /* op */
    memcpy(reply + 12, msg + 12, 2); /* sequence */
    CHECK_EQ(server_send(k_ack, sizeof k_ack, now), XMSG_OK);
    CHECK_EQ(server_send(reply, sizeof reply, now), XMSG_OK);
}

/* Whether our message is the captured one's kind: the same type and length, and for a request
 * the same op. After a cancel the client still sends what the captured client sent for the
 * work already under way (short acks, the halves of a block); its next request differs. */
static bool same_kind(const uint8_t *ours, size_t len, const char *captured_hex)
{
    uint8_t want[FA_DATA_MESSAGE_SIZE];
    int n = test_hex_to_bytes(captured_hex, want, sizeof want);
    if (n < 2 || (size_t)n != len || ours[0] != want[0] || ours[1] != want[1])
    {
        return false;
    }
    if (ours[0] == 0x07 && ours[1] == 0xF0 && len != FA_DATA_MESSAGE_SIZE && len > 10u)
    {
        return ours[9] == want[9] && ours[10] == want[10];
    }
    return true;
}

/* Whether a captured event is a client request (not a short ack, not a data message). */
static bool is_client_request(const FaVectorEvent *e)
{
    uint8_t b[FA_DATA_MESSAGE_SIZE];
    int n = test_hex_to_bytes(e->hex, b, sizeof b);
    return e->who == 'C' && n >= 2 && n != (int)FA_DATA_MESSAGE_SIZE &&
           ((b[0] == 0x07 && b[1] == 0xF0) || (b[0] == 0x07 && b[1] == 0x82));
}

/* Play a capture until the client is cancelled, then let the test server answer the teardown.
 * Every client message is compared with the captured one while they are of the same kind; the
 * captured server goes on answering them, as a real server finishes the work under way. From
 * the first client message the capture does not have (its first teardown request), the test
 * server answers instead, and the captured server says nothing more. */
static void cancel_run(const FaVectorEvent *ev, size_t n, const char *spec, CancelAt at)
{
    FaClientConfig cfg;
    uint32_t now = bring_up();
    uint32_t until = now + 60000u;
    size_t c_idx = next_client_event(ev, n, 0);
    size_t s_idx = 0;
    bool off_script = false;
    uint8_t body[FA_DATA_MESSAGE_SIZE];

    memset(&s_down, 0, sizeof s_down);
    s_sink.cancel_at_entry = at.at_entry;
    s_sink.cancel_at_data = at.at_first_data;
    s_sink.cancel_at_offset = at.at_offset;
    fa_client_config_default(&cfg);
    if (s_run.asker != NULL)
    {
        snprintf(cfg.asker, sizeof cfg.asker, "%s", s_run.asker);
    }
    client_start(&cfg, spec, now);
    if (at.at_start)
    {
        s_sink.cancelled = true;
        CHECK_EQ(fa_client_cancel(&s_client, now), XMSG_OK);
    }
    while (now < until && s_sink.done_calls == 0)
    {
        now++;
        pump(now);
        while (s_server.count > 0)
        {
            size_t slot = s_server.head;
            const uint8_t *m = s_server.bodies[slot];
            size_t len = s_server.lens[slot];
            if (!off_script && c_idx < n && s_sink.cancelled && !same_kind(m, len, ev[c_idx].hex))
            {
                off_script = true;
            }
            if (off_script)
            {
                answer_teardown(m, len, now);
            }
            else if (c_idx < n)
            {
                check_client_message(m, len, &s_server.subs[slot], ev[c_idx].hex);
                c_idx = next_client_event(ev, n, c_idx + 1u);
            }
            s_server.head = (s_server.head + 1u) % MAX_SEEN;
            s_server.count--;
        }
        while (!off_script && s_idx < n)
        {
            int len;
            if (ev[s_idx].who == 'C')
            {
                if (s_idx >= c_idx || (s_sink.cancelled && is_client_request(&ev[s_idx])))
                {
                    break; /* not come yet, or a request the cancelled client will not send */
                }
                s_idx++;
                continue;
            }
            len = test_hex_to_bytes(ev[s_idx].hex, body, sizeof body);
            if (server_send(body, (size_t)len, now) != XMSG_OK)
            {
                break; /* window full: next millisecond */
            }
            s_idx++;
        }
    }
    /* Let the last acknowledgements settle. */
    for (uint32_t i = 0; i < 200u; i++)
    {
        now++;
        pump(now);
    }
}

static void cancelled_listing_gives_the_seat_back(void)
{
    s_run = (Run){true, "SYS", "BAK04", 0, true, false};
    cancel_run(k_fa_list_events, N_LIST, "", (CancelAt){false, 3, false, 0});
    s_run = (Run){false, NULL, NULL, 0, false, false};

    CHECK(s_sink.cancelled);
    CHECK_EQ(s_sink.done_calls, 1);
    CHECK_EQ(s_sink.result, XMSG_ECANCELED);
    CHECK_EQ(s_sink.entry_count, 3); /* no entry after the cancel */
    CHECK_EQ(s_down.op_count, 1);
    CHECK_EQ(s_down.ops[0], FA_OP_RELEASE);
    CHECK_EQ(s_down.finished, 1);
    CHECK_EQ(fa_client_state(&s_client), FA_CLIENT_DONE);
    CHECK(!s_client.base.attached);
}

static void cancelled_read_closes_the_file(void)
{
    cancel_run(k_fa_read_events, N_READ, "PATCH-FILE:OUT", (CancelAt){false, 0, true, 0});

    CHECK(s_sink.cancelled);
    CHECK_EQ(s_sink.done_calls, 1);
    CHECK_EQ(s_sink.result, XMSG_ECANCELED);
    CHECK_EQ(s_sink.opened_calls, 1);
    CHECK_EQ(s_sink.data_calls, 1); /* the block's second half came after the cancel */
    CHECK_EQ(s_sink.got, FA_DATA_BYTES);
    CHECK_EQ(s_down.op_count, 2);
    CHECK_EQ(s_down.ops[0], FA_OP_CLOSE);
    CHECK_EQ(s_down.ops[1], FA_OP_RELEASE);
    CHECK_EQ(s_down.finished, 1);
    CHECK_EQ(fa_client_state(&s_client), FA_CLIENT_DONE);
    CHECK(!s_client.base.attached);
}

static void cancelled_before_the_confirm(void)
{
    /* The captured confirm still comes (the letter is what the capture has); then only
     * session finished goes out. */
    cancel_run(k_fa_read_events, N_READ, "PATCH-FILE:OUT", (CancelAt){true, 0, false, 0});

    CHECK_EQ(s_sink.done_calls, 1);
    CHECK_EQ(s_sink.result, XMSG_ECANCELED);
    CHECK_EQ(s_sink.opened_calls, 0);
    CHECK_EQ(s_down.op_count, 0);
    CHECK_EQ(s_down.finished, 1);
    CHECK_EQ(fa_client_state(&s_client), FA_CLIENT_DONE);
}

/* The captured write's content: the 1024-byte payloads of its data messages, in order, cut at
 * the length its set end of file declared (17904 + 1). */
static void write_file_load(void)
{
    uint8_t body[FA_DATA_MESSAGE_SIZE];
    uint32_t at = 0;
    for (size_t i = 0; i < N_WRITE; i++)
    {
        int len = test_hex_to_bytes(k_fa_write_events[i].hex, body, sizeof body);
        if (k_fa_write_events[i].who == 'C' && len == (int)FA_DATA_MESSAGE_SIZE)
        {
            uint32_t n = WRITE_LENGTH - at;
            if (n > FA_DATA_BYTES)
            {
                n = FA_DATA_BYTES;
            }
            memcpy(s_write_file + at, body + FA_ENVELOPE_SIZE, n);
            at += n;
        }
    }
    CHECK_EQ(at, WRITE_LENGTH);
}

static void write_requests_match_the_capture(void)
{
    uint8_t out[FA_REQUEST_MAX];
    uint8_t data[FA_DATA_MESSAGE_SIZE];
    uint8_t bytes[FA_DATA_BYTES];
    FaEnvelope env = {FA_TYPE_REQUEST, 0x0044, 0x9E, FA_TOKEN_CLIENT};
    size_t n;

    /* 02:33:04.663: set end of file for 17905 bytes carries 45F0. */
    n = fa_build_set_end_of_file(out, sizeof out, &env, 0x000D, WRITE_LENGTH);
    CHECK_BYTES(out, n, "07F000449E00D76192000C92000DF2000192003BF200028C8005A4000045F0F200FF");
    CHECK_EQ(fa_build_set_end_of_file(out, sizeof out, &env, 1, 0), 0);
    /* 02:33:02.035, pad byte aside: open WRTEST1:OUT for writing. */
    env.counter = 0x81;
    n = fa_build_open_write(out, sizeof out, &env, 0x0002, "WRTEST1:OUT", true);
    CHECK_BYTES(out, n,
                "07F000448100D761920005920002F20002BF22575254455354313A4F5554222757F20003"
                "920001F200FF");
    /* Overwrite: the same open with the name bare (RetroIO LiveSyncTransferAgent). */
    n = fa_build_open_write(out, sizeof out, &env, 0x0002, "WRTEST1:OUT", false);
    CHECK_BYTES(out, n,
                "07F000448100D761920005920002F20002BD575254455354313A4F55542757F20003"
                "920001F200FF");
    CHECK(strcmp(fa_error_name(FA_ERR_FILE_EXISTS), "file already exists") == 0);
    /* A data message is the envelope and the bytes, nothing else. */
    memset(bytes, 0x5A, sizeof bytes);
    env.counter = 0x04;
    CHECK_EQ(fa_build_data_message(data, sizeof data, &env, bytes), FA_DATA_MESSAGE_SIZE);
    CHECK_EQ(data[4], 0x04);
    CHECK_EQ(data[8], 0x5A);
    CHECK_EQ(fa_build_data_message(data, FA_DATA_MESSAGE_SIZE - 1u, &env, bytes), 0);
}

static void write_replays_the_capture(void)
{
    write_file_load();
    s_run = (Run){false, NULL, NULL, 0, false, true};
    CHECK(replay(k_fa_write_events, N_WRITE, "WRTEST1:OUT", false));
    s_run = (Run){false, NULL, NULL, 0, false, false};

    CHECK_EQ(s_sink.done_calls, 1);
    CHECK_EQ(s_sink.result, 0);
    CHECK_EQ(s_sink.sintran_error, 0);
    CHECK_EQ(s_sink.opened_calls, 0);
    CHECK_EQ(s_client.file_number, 0x40); /* the open reply, F2 0002 A2 0040 */
    CHECK_EQ(s_client.blocks, 9);
    CHECK_EQ(fa_client_state(&s_client), FA_CLIENT_DONE);
    CHECK(!s_client.base.attached);
    CHECK_EQ(s_client.stats.ignored, 0);
}

static void cancelled_write_closes_the_file(void)
{
    /* Inside the second half of block 1: that block is finished, then close and release, with
     * no set end of file. */
    write_file_load();
    s_run = (Run){false, NULL, NULL, 0, false, true};
    cancel_run(k_fa_write_events, N_WRITE, "WRTEST1:OUT",
               (CancelAt){false, 0, false, FA_BLOCK_SIZE + FA_DATA_BYTES});
    s_run = (Run){false, NULL, NULL, 0, false, false};

    CHECK(s_sink.cancelled);
    CHECK_EQ(s_sink.done_calls, 1);
    CHECK_EQ(s_sink.result, XMSG_ECANCELED);
    CHECK_EQ(s_down.op_count, 2);
    CHECK_EQ(s_down.ops[0], FA_OP_CLOSE);
    CHECK_EQ(s_down.ops[1], FA_OP_RELEASE);
    CHECK_EQ(s_down.finished, 1);
    CHECK_EQ(fa_client_state(&s_client), FA_CLIENT_DONE);
    CHECK(!s_client.base.attached);
}

static void write_source_that_fails(void)
{
    FaClientConfig cfg;
    uint32_t now = bring_up();
    uint32_t until = now + 60000u;
    size_t s_idx = 0;
    uint8_t body[FA_DATA_MESSAGE_SIZE];

    write_file_load();
    s_sink.fail_at_offset = FA_DATA_BYTES; /* the second half of block 0 */
    s_run = (Run){false, NULL, NULL, 0, false, true};
    fa_client_config_default(&cfg);
    client_start(&cfg, "WRTEST1:OUT", now);
    s_run = (Run){false, NULL, NULL, 0, false, false};
    /* The captured server answers the first steps; the client stops at the bad half. */
    while (now < until && s_sink.done_calls == 0)
    {
        now++;
        pump(now);
        s_server.head = (s_server.head + s_server.count) % MAX_SEEN;
        s_server.count = 0;
        while (s_idx < 13u)
        {
            int len;
            if (k_fa_write_events[s_idx].who == 'C')
            {
                s_idx++;
                continue;
            }
            len = test_hex_to_bytes(k_fa_write_events[s_idx].hex, body, sizeof body);
            if (server_send(body, (size_t)len, now) != XMSG_OK)
            {
                break;
            }
            s_idx++;
        }
    }
    CHECK_EQ(s_sink.done_calls, 1);
    CHECK_EQ(s_sink.result, XMSG_EIO);
    CHECK_EQ(fa_client_state(&s_client), FA_CLIENT_FAILED);
    CHECK(!s_client.base.attached);
}

static void write_arguments_are_refused(void)
{
    FaClient c;
    FaClientCallbacks cb;
    memset(&cb, 0, sizeof cb);
    (void)fa_client_init(&c, NULL, &cb);
    /* No source. */
    CHECK_EQ(fa_client_write(&c, &s_a, 102, 102, "A", "A", "", "X:Y", 10, FA_WRITE_CREATE, 0),
             XMSG_EINVAL);
    cb.source = sink_source;
    (void)fa_client_init(&c, NULL, &cb);
    /* An empty file: no capture shows one being written. */
    CHECK_EQ(fa_client_write(&c, &s_a, 102, 102, "A", "A", "", "X:Y", 0, FA_WRITE_CREATE, 0),
             XMSG_EINVAL);
    CHECK_EQ(fa_client_write(&c, &s_a, 102, 102, "A", "A", "", "", 10, FA_WRITE_CREATE, 0),
             XMSG_EINVAL);
    CHECK_EQ(fa_client_write(&c, &s_a, 102, 102, "A", "A", "", "X:Y", 10, (FaWriteMode)7, 0),
             XMSG_EINVAL);
    CHECK_EQ(fa_client_state(&c), FA_CLIENT_IDLE);
}

static void cancel_when_nothing_runs(void)
{
    FaClient c;
    int done_before;
    (void)fa_client_init(&c, NULL, NULL);
    CHECK_EQ(fa_client_cancel(NULL, 0), XMSG_EINVAL);
    CHECK_EQ(fa_client_cancel(&c, 0), XMSG_OK);
    CHECK_EQ(fa_client_state(&c), FA_CLIENT_IDLE);
    /* After a finished read: nothing changes and no second `done`. */
    CHECK(replay(k_fa_read_events, N_READ, "PATCH-FILE:OUT", false));
    done_before = s_sink.done_calls;
    CHECK_EQ(fa_client_cancel(&s_client, 0), XMSG_OK);
    pump(100000u);
    CHECK_EQ(s_sink.done_calls, done_before);
    CHECK_EQ(s_sink.result, 0);
    CHECK_EQ(fa_client_state(&s_client), FA_CLIENT_DONE);
}

/* ------------------------------------------------------------------ text forms */

static void access_text(void)
{
    char t[FA_TEXT_ACCESS_WORDS];

    /* 003777: D100's FILE-STAT of STARTX3:MODE; the letters as SINTRAN's FTP server prints. */
    CHECK_EQ(fa_text_access_letters(03777, t, sizeof t), 15);
    CHECK(strcmp(t, "rwacdrwacdr----") == 0);
    /* The second measured FTP line: own RWAD, friend RWA, public R. */
    CHECK_EQ(fa_text_access_letters(0x17u | (0x07u << 5) | (0x01u << 10), t, sizeof t), 15);
    CHECK(strcmp(t, "rwa-drwa--r----") == 0);
    CHECK_EQ(fa_text_access_letters(0, t, 15), -1);

    CHECK_EQ(fa_text_access_words(03777, FA_ACCESS_OWN_SHIFT, t, sizeof t), 38);
    CHECK(strcmp(t, "READ, WRITE, APPEND, COMMON, DIRECTORY") == 0);
    CHECK_EQ(fa_text_access_words(03777, FA_ACCESS_PUBLIC_SHIFT, t, sizeof t), 4);
    CHECK(strcmp(t, "READ") == 0);
    /* RetroIO's SINTRAN printout: OWN READ, WRITE, APPEND; FRIEND and PUBLIC NONE. */
    CHECK_EQ(fa_text_access_words(0x07u, FA_ACCESS_OWN_SHIFT, t, sizeof t), 19);
    CHECK(strcmp(t, "READ, WRITE, APPEND") == 0);
    CHECK_EQ(fa_text_access_words(0x07u, FA_ACCESS_FRIEND_SHIFT, t, sizeof t), 4);
    CHECK(strcmp(t, "NONE") == 0);
    CHECK_EQ(fa_text_access_words(0x07u, 3, t, sizeof t), -1);
    CHECK_EQ(fa_text_access_words(03777, FA_ACCESS_OWN_SHIFT, t, 20), -1);
}

static void date_text(void)
{
    char t[FA_TEXT_DATE_STAT];

    /* 0xC1FAAA75: 1998, July 29, 10.41.53. */
    CHECK_EQ(fa_text_date_stat(0xC1FAAA75u, t, sizeof t), 23);
    CHECK(strcmp(t, "10.41.53  JULY 29, 1998") == 0);
    CHECK_EQ(fa_text_date_stat(0, t, sizeof t), 0);
    CHECK_EQ(t[0], '\0');
    CHECK_EQ(fa_text_date_ftp(0xC1FAAA75u, t, sizeof t), 15);
    CHECK(strcmp(t, "29 Jul 98 10:41") == 0);
    CHECK_EQ(fa_text_date_ftp(0, t, sizeof t), 15);
    CHECK(strcmp(t, "00 pre 50 00:00") == 0);
    CHECK_EQ(fa_text_date_ftp(0, t, 15), -1);
}

static void name_text(void)
{
    FaFileEntry e;
    char t[FA_TEXT_NAME];

    memset(&e, 0, sizeof e);
    memcpy(e.name, "AIP-CONFIG", 11);
    e.object_index = 59;
    e.next_version = 59;
    CHECK_EQ(fa_text_name(&e, FA_NAME_SINTRAN, t, sizeof t), 11);
    CHECK(strcmp(t, "AIP-CONFIG:") == 0); /* no type */
    CHECK_EQ(fa_text_name(&e, FA_NAME_FTP_VERSION, t, sizeof t), 13);
    CHECK(strcmp(t, "AIP-CONFIG.-1") == 0);
    memcpy(e.type, "SYMB", 5);
    CHECK_EQ(fa_text_name(&e, FA_NAME_SINTRAN_VERSION, t, sizeof t), 17);
    CHECK(strcmp(t, "AIP-CONFIG:SYMB;1") == 0);
    e.next_version = 60; /* another version: its number is not known */
    CHECK_EQ(fa_text_version(&e), 0);
    CHECK_EQ(fa_text_name(&e, FA_NAME_SINTRAN_VERSION, t, sizeof t), -1);
    CHECK_EQ(fa_text_name(&e, FA_NAME_FTP, t, sizeof t), 15);
    CHECK(strcmp(t, "AIP-CONFIG.SYMB") == 0);

    CHECK_EQ(fa_text_name_from_ftp("ECHOPL.PLNC", t, sizeof t), 11);
    CHECK(strcmp(t, "ECHOPL:PLNC") == 0);
    CHECK_EQ(fa_text_name_from_ftp("ECHOPL.PLNC-1", t, sizeof t), 11);
    CHECK(strcmp(t, "ECHOPL:PLNC") == 0);
    CHECK_EQ(fa_text_name_from_ftp("LOAD-MODE.BATC-12", t, sizeof t), 14);
    CHECK(strcmp(t, "LOAD-MODE:BATC") == 0);
    CHECK_EQ(fa_text_name_from_ftp("NAME.", t, sizeof t), 5);
    CHECK(strcmp(t, "NAME:") == 0);
    CHECK_EQ(fa_text_name_from_ftp("A.B-X", t, sizeof t), 5); /* not a version */
    CHECK(strcmp(t, "A:B-X") == 0);
    CHECK_EQ(fa_text_name_from_ftp("NAME:TYPE;1", t, sizeof t), 11);
    CHECK(strcmp(t, "NAME:TYPE;1") == 0);
    CHECK_EQ(fa_text_name_from_ftp("ECHO", t, sizeof t), 4);
    CHECK(strcmp(t, "ECHO") == 0);
    CHECK_EQ(fa_text_name_from_ftp("ECHOPL.PLNC", t, 11), -1);
}

static void bad_arguments_are_refused(void)
{
    FaClient c;
    (void)fa_client_init(&c, NULL, NULL);
    CHECK_EQ(fa_client_read(&c, NULL, 102, 102, "A", "A", "", "X:Y", 0), XMSG_EINVAL);
    CHECK_EQ(fa_client_read(&c, &s_a, 0, 102, "A", "A", "", "X:Y", 0), XMSG_EINVAL);
    CHECK_EQ(fa_client_read(&c, &s_a, 102, 102, "A", "A", "", "", 0), XMSG_EINVAL);
    CHECK_EQ(fa_client_read(&c, &s_a, 102, 102, NULL, "A", "", "X:Y", 0), XMSG_EINVAL);
    CHECK_EQ(fa_client_state(&c), FA_CLIENT_IDLE);
}

int main(void)
{
    password_fold_vectors();
    qform_walks_into_constructed_values();
    qform_writer_forms();
    replies_are_read();
    builders_refuse_what_does_not_fit();
    read_replays_the_capture();
    refused_open_still_gives_the_seat_back();
    repeated_confirm_changes_nothing();
    listing_replays_the_capture();
    file_statistics_replays_the_capture();
    walk_requests_and_dates();
    silent_server_times_out();
    cancelled_listing_gives_the_seat_back();
    cancelled_read_closes_the_file();
    cancelled_before_the_confirm();
    cancel_when_nothing_runs();
    write_requests_match_the_capture();
    write_replays_the_capture();
    cancelled_write_closes_the_file();
    write_source_that_fails();
    write_arguments_are_refused();
    access_text();
    date_text();
    name_text();
    bad_arguments_are_refused();
    TEST_DONE();
}
