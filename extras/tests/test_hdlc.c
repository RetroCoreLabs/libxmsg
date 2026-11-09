/*
 * test_hdlc.c - CRC-16/X.25, the bit-stuffing encoder, the decoder and the control field.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Expected values: CRC-16/X.25 of "123456789" is 0x906E, computed with a bit-serial Python
 * loop (poly 0x8408 reflected, init 0xFFFF, final XOR 0xFFFF) on 2026-11-09. The round trips
 * check the encoder and decoder against each other AND against the wire rules they must obey
 * (no six ones in a row inside a frame), so a bug shared by both halves still shows.
 */
#include "libhdlc/hdlc.h"
#include "test_util.h"

/* ---------------------------------------------------------- frame sink */

typedef struct
{
    uint8_t bytes[2048];
    int count;
    int frames;
    int good;
    int bad;
    int aborts;
    int discards;
    hdlc_rx_status last_status;
    uint8_t last[2048];
    int last_len;
} Sink;

static void sink_byte(void *ctx, uint8_t data)
{
    Sink *s = (Sink *)ctx;
    if (s->count < (int)sizeof s->bytes)
    {
        s->bytes[s->count] = data;
    }
    s->count++;
}

static void sink_end(void *ctx, hdlc_rx_status status)
{
    Sink *s = (Sink *)ctx;
    s->frames++;
    s->last_status = status;
    if (status == HDLC_RX_OK)
    {
        s->good++;
    }
    else
    {
        s->bad++;
    }
    s->last_len = (s->count < (int)sizeof s->last) ? s->count : (int)sizeof s->last;
    memcpy(s->last, s->bytes, (size_t)s->last_len);
    s->count = 0;
}

static void sink_abort(void *ctx)
{
    Sink *s = (Sink *)ctx;
    s->aborts++;
    s->count = 0;
}

static void sink_discard(void *ctx)
{
    Sink *s = (Sink *)ctx;
    s->discards++;
    s->count = 0;
}

static hdlc_frame_sink make_sink(Sink *s)
{
    hdlc_frame_sink k;
    memset(s, 0, sizeof *s);
    k.ctx = s;
    k.on_byte = sink_byte;
    k.on_end = sink_end;
    k.on_abort = sink_abort;
    k.on_discard = sink_discard;
    return k;
}

/* --------------------------------------------------------- line helper */

typedef struct
{
    uint8_t b[8192];
    int n;
} Line;

static void line_put(Line *l, const uint8_t *p, int n)
{
    for (int i = 0; i < n; i++)
    {
        l->b[l->n++] = p[i];
    }
}

/** Encode one frame (payload + computed FCS) onto the line. */
static void encode_frame(Line *l, const uint8_t *payload, int len)
{
    hdlc_encoder enc;
    uint8_t out[4];
    hdlc_encoder_reset(&enc);
    line_put(l, out, hdlc_encoder_open(&enc, out));
    for (int i = 0; i < len; i++)
    {
        line_put(l, out, hdlc_encoder_write(&enc, payload[i], out));
    }
    line_put(l, out, hdlc_encoder_write_fcs(&enc, out));
    line_put(l, out, hdlc_encoder_close(&enc, out));
}

/** Longest run of ones on the line between the first and last flag, bit order LSB first. */
static int longest_ones_inside(const Line *l)
{
    int run = 0;
    int best = 0;
    /* Skip the first and the last byte: they hold the flags and idle padding. */
    for (int i = 1; i < l->n - 1; i++)
    {
        for (int bit = 0; bit < 8; bit++)
        {
            if ((l->b[i] >> bit) & 1u)
            {
                run++;
                best = (run > best) ? run : best;
            }
            else
            {
                run = 0;
            }
        }
    }
    return best;
}

/** Copy the line shifted by k bit times (k idle ones first), padded with ones. */
static void shift_line(const Line *in, int k, Line *out)
{
    int total = (in->n * 8) + k;
    out->n = (total + 7) / 8 + 1;
    memset(out->b, 0xFF, (size_t)out->n);
    for (int i = 0; i < in->n * 8; i++)
    {
        int bit = (in->b[i / 8] >> (i % 8)) & 1;
        int pos = i + k;
        if (bit == 0)
        {
            out->b[pos / 8] = (uint8_t)(out->b[pos / 8] & ~(1u << (pos % 8)));
        }
    }
}

static void feed_line(const Line *l, hdlc_decoder *dec, const hdlc_frame_sink *k)
{
    for (int i = 0; i < l->n; i++)
    {
        hdlc_decoder_feed(dec, l->b[i], k);
    }
}

/* --------------------------------------------------------------- tests */

static void crc_vectors(void)
{
    uint16_t crc = HDLC_CRC_INIT;
    const char *s = "123456789";
    for (int i = 0; s[i] != '\0'; i++)
    {
        crc = hdlc_crc_byte(crc, (uint8_t)s[i]);
    }
    CHECK_EQ(crc ^ 0xFFFFu, 0x906Eu);

    /* Appending the FCS (inverted CRC, low byte first) leaves the good residue. */
    {
        uint16_t fcs = (uint16_t)(crc ^ 0xFFFFu);
        crc = hdlc_crc_byte(crc, (uint8_t)(fcs & 0xFFu));
        crc = hdlc_crc_byte(crc, (uint8_t)(fcs >> 8u));
        CHECK_EQ(crc, HDLC_CRC_GOOD);
    }
}

static void round_trip_every_offset(void)
{
    static const uint8_t frames[][8] = {
        { 0x01, 0x3F },                                     /* SABM with P */
        { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }, /* every bit stuffed */
        { 0x7E, 0x7E, 0x7D, 0x7F, 0x00, 0x7E, 0xFE, 0x3F }, /* flags and aborts as data */
        { 0x03, 0x00, 0x1F, 0xF8, 0x0F, 0xF0, 0x55, 0xAA },
    };
    static const int lens[] = { 2, 8, 8, 8 };

    for (int f = 0; f < 4; f++)
    {
        Line l;
        l.n = 0;
        encode_frame(&l, frames[f], lens[f]);
        CHECK(longest_ones_inside(&l) <= 6);    /* the closing flag's six ones may show */

        for (int k = 0; k < 8; k++)
        {
            Line shifted;
            hdlc_decoder dec;
            Sink s;
            hdlc_frame_sink sink = make_sink(&s);
            shift_line(&l, k, &shifted);
            hdlc_decoder_reset(&dec);
            feed_line(&shifted, &dec, &sink);
            CHECK_EQ(s.frames, 1);
            CHECK_EQ(s.good, 1);
            CHECK_EQ(s.last_len, lens[f] + 2);
            CHECK(memcmp(s.last, frames[f], (size_t)lens[f]) == 0);
        }
    }
}

static void long_random_frames(void)
{
    uint32_t seed = 12345u;
    for (int round = 0; round < 50; round++)
    {
        uint8_t payload[622];
        int len = 1 + (int)(seed % 622u);
        Line l;
        hdlc_decoder dec;
        Sink s;
        hdlc_frame_sink sink = make_sink(&s);
        for (int i = 0; i < len; i++)
        {
            seed = seed * 1103515245u + 12345u;
            payload[i] = (uint8_t)(seed >> 16u);
        }
        l.n = 0;
        encode_frame(&l, payload, len);
        hdlc_decoder_reset(&dec);
        feed_line(&l, &dec, &sink);
        CHECK_EQ(s.good, 1);
        CHECK_EQ(s.last_len, len + 2);
        CHECK(memcmp(s.last, payload, (size_t)len) == 0);
    }
}

static void back_to_back_frames_no_phantoms(void)
{
    static const uint8_t a[] = { 0x01, 0x73 };          /* UA with F */
    static const uint8_t b[] = { 0x03, 0x00, 0x42, 0x43 };
    Line l;
    hdlc_decoder dec;
    Sink s;
    hdlc_frame_sink sink = make_sink(&s);
    l.n = 0;
    for (int i = 0; i < 20; i++)
    {
        encode_frame(&l, a, 2);
        encode_frame(&l, b, 4);
    }
    hdlc_decoder_reset(&dec);
    feed_line(&l, &dec, &sink);
    CHECK_EQ(s.frames, 40);
    CHECK_EQ(s.good, 40);
    CHECK_EQ(s.bad, 0);
}

static void corrupted_fcs_is_bad(void)
{
    static const uint8_t p[] = { 0x01, 0x00, 0x10, 0x20 };
    hdlc_encoder enc;
    hdlc_decoder dec;
    Sink s;
    hdlc_frame_sink sink = make_sink(&s);
    uint8_t out[4];
    Line l;
    l.n = 0;
    hdlc_encoder_reset(&enc);
    line_put(&l, out, hdlc_encoder_open(&enc, out));
    for (int i = 0; i < 4; i++)
    {
        line_put(&l, out, hdlc_encoder_write(&enc, p[i], out));
    }
    /* A wrong FCS written as plain data. */
    line_put(&l, out, hdlc_encoder_write(&enc, 0x12, out));
    line_put(&l, out, hdlc_encoder_write(&enc, 0x34, out));
    line_put(&l, out, hdlc_encoder_close(&enc, out));
    hdlc_decoder_reset(&dec);
    feed_line(&l, &dec, &sink);
    CHECK_EQ(s.frames, 1);
    CHECK_EQ(s.bad, 1);
}

static void abort_drops_frame(void)
{
    static const uint8_t p[] = { 0x01, 0x00, 0x10, 0x20, 0x30 };
    hdlc_encoder enc;
    hdlc_decoder dec;
    Sink s;
    hdlc_frame_sink sink = make_sink(&s);
    uint8_t out[4];
    Line l;
    l.n = 0;
    hdlc_encoder_reset(&enc);
    line_put(&l, out, hdlc_encoder_open(&enc, out));
    for (int i = 0; i < 5; i++)
    {
        line_put(&l, out, hdlc_encoder_write(&enc, p[i], out));
    }
    line_put(&l, out, hdlc_encoder_abort(&enc, out));
    encode_frame(&l, p, 5);     /* a good frame afterwards still arrives */
    hdlc_decoder_reset(&dec);
    feed_line(&l, &dec, &sink);
    CHECK_EQ(s.aborts, 1);
    CHECK_EQ(s.good, 1);
    CHECK_EQ(s.bad, 0);
}

static void control_field(void)
{
    hdlc_control_field c = hdlc_decode_control(0x3F);
    CHECK_EQ(c.type, HDLC_FRAME_U);
    CHECK(strcmp(c.name, "SABM") == 0);
    CHECK(c.poll_final);

    c = hdlc_decode_control(0x63);
    CHECK_EQ(c.type, HDLC_FRAME_U);
    CHECK(strcmp(c.name, "UA") == 0);
    CHECK(!c.poll_final);

    c = hdlc_decode_control(0xA1);     /* RR, N(R) = 5 */
    CHECK_EQ(c.type, HDLC_FRAME_S);
    CHECK(strcmp(c.name, "RR") == 0);
    CHECK_EQ(c.nr, 5);

    c = hdlc_decode_control(0x46);     /* I, N(S) = 3, N(R) = 2 */
    CHECK_EQ(c.type, HDLC_FRAME_I);
    CHECK_EQ(c.ns, 3);
    CHECK_EQ(c.nr, 2);
}

/*
 * Idle line, then a byte whose low six bits are ones and bit 6 is zero (0x3F): on the wire that
 * is 8 + 6 = 14 ones and then a zero, which is NOT a flag (a flag is exactly 0 111111 0). Data
 * bytes and a closing flag follow; if the decoder took 0x3F as an opening flag it delivers a
 * frame. Found by reading hdlc_decoder_feed() on 2026-11-09: the idle shortcut skipped the
 * 0xFF without counting its ones. RC422Dev hit the same class of bug (hunt register started at
 * zero, measured 2026-11-08).
 */
static void idle_then_six_ones_is_not_a_flag(void)
{
    static const uint8_t line[] = { 0xFF, 0x3F, 0x00, 0x12, 0x34, 0x56, 0x78, 0x7E, 0xFF };
    hdlc_decoder dec;
    Sink s;
    hdlc_frame_sink sink = make_sink(&s);
    Line l;
    l.n = 0;
    line_put(&l, line, (int)sizeof line);
    hdlc_decoder_reset(&dec);
    feed_line(&l, &dec, &sink);
    CHECK_EQ(s.frames, 0);
}

/* ---------------------------------------------- table, status, limits */

static void run_table_matches_rule(void)
{
    const uint8_t *t = hdlc_run_table();
    for (int d = 0; d < 256; d++)
    {
        int bits[8];
        int l = 0;
        int tr = 0;
        int run = 0;
        bool ok = true;
        uint8_t want;
        for (int i = 0; i < 8; i++)
        {
            bits[i] = (d >> i) & 1;
        }
        if (d == 0xFF)
        {
            CHECK_EQ(t[d], 0);
            continue;
        }
        while (bits[l] == 1)
        {
            l++;
        }
        while (bits[7 - tr] == 1)
        {
            tr++;
        }
        for (int i = l + 1; i < 8; i++)
        {
            if (bits[i] != 0)
            {
                run++;
            }
            else
            {
                if (run > 4)
                {
                    ok = false;
                }
                run = 0;
            }
        }
        if (tr > 6)
        {
            ok = false;
        }
        want = (uint8_t)((ok ? 0x80u : 0u) | ((unsigned)tr << 3u) | (unsigned)l);
        CHECK_EQ(t[d], want);
    }
}

/** Put n raw bits (LSB first of v) on a bit line, no stuffing. Line starts all ones. */
static void bits_put(uint8_t *line, int *pos, uint32_t v, int n)
{
    for (int i = 0; i < n; i++)
    {
        if (((v >> i) & 1u) == 0)
        {
            line[*pos / 8] = (uint8_t)(line[*pos / 8] & ~(1u << (*pos % 8)));
        }
        (*pos)++;
    }
}

/** Put data bits with a stuffed zero after every five ones in a row. */
static void bits_put_stuffed(uint8_t *line, int *pos, int *ones, uint32_t v, int n)
{
    for (int i = 0; i < n; i++)
    {
        uint32_t bit = (v >> i) & 1u;
        if (*ones == 5)
        {
            bits_put(line, pos, 0u, 1);
            *ones = 0;
        }
        bits_put(line, pos, bit, 1);
        *ones = (bit != 0) ? (*ones + 1) : 0;
    }
}

static void odd_bits_reported(void)
{
    /* flag, 5 bytes + 3 extra data bits, flag: not a whole number of bytes. */
    uint8_t line[32];
    int pos = 0;
    int ones = 0;
    hdlc_decoder dec;
    Sink s;
    hdlc_frame_sink sink = make_sink(&s);
    memset(line, 0xFF, sizeof line);
    bits_put(line, &pos, 0x7Eu, 8);
    bits_put_stuffed(line, &pos, &ones, 0x04030201u, 32);
    bits_put_stuffed(line, &pos, &ones, 0x05u, 8);
    bits_put_stuffed(line, &pos, &ones, 0x2u, 3);
    bits_put(line, &pos, 0x7Eu, 8);
    hdlc_decoder_reset(&dec);
    for (int i = 0; i < (pos + 7) / 8 + 1; i++)
    {
        hdlc_decoder_feed(&dec, line[i], &sink);
    }
    CHECK_EQ(s.frames, 1);
    CHECK_EQ(s.last_status, HDLC_RX_ODD_BITS);
    CHECK_EQ(dec.stats.odd_bits, 1);
    CHECK_EQ(dec.stats.bad_crc, 0);
}

static void too_long_cut_and_counted(void)
{
    uint8_t payload[700];
    Line l;
    hdlc_decoder dec;
    Sink s;
    hdlc_frame_sink sink = make_sink(&s);
    for (int i = 0; i < 700; i++)
    {
        payload[i] = (uint8_t)(i * 7);
    }
    l.n = 0;
    encode_frame(&l, payload, 624);     /* 626 with FCS: exactly the limit */
    encode_frame(&l, payload, 625);     /* 627: one byte over */
    encode_frame(&l, payload, 10);      /* and the decoder carries on */
    hdlc_decoder_reset(&dec);
    hdlc_decoder_set_max_bytes(&dec, 626);
    feed_line(&l, &dec, &sink);
    CHECK_EQ(s.frames, 3);
    CHECK_EQ(dec.stats.ok, 2);
    CHECK_EQ(dec.stats.too_long, 1);
    CHECK_EQ(dec.stats.bytes, 626 + 626 + 12);
    CHECK_EQ(s.last_status, HDLC_RX_OK);
    CHECK(strcmp(hdlc_rx_status_name(HDLC_RX_TOO_LONG), "TOO_LONG") == 0);
}

/* -------------------------------------------- reference decoder (spec) */

/*
 * A second receiver written straight from the ISO 13239 bit rules, one bit at a time and as
 * plain as possible, recording every event as text. For any line the library must produce the
 * same text. This is what shows the whole-byte fast path changes nothing.
 * Events: data bytes as hex, " E<status> " frame end, "A<n> " abort after n bytes,
 * " s<n> " short run of n bytes discarded.
 */
typedef struct
{
    char log[65536];
    int n;
} Events;

/** Append one event: kind x = data byte, E = frame end, A = abort, s = short run. */
static void ev(Events *e, char kind, int v)
{
    size_t room;
    if (e->n >= (int)sizeof e->log - 32)
    {
        return;
    }
    room = sizeof e->log - (size_t)e->n;
    switch (kind)
    {
    case 'x':
        e->n += snprintf(&e->log[e->n], room, "%02X", v);
        break;
    case 'E':
        e->n += snprintf(&e->log[e->n], room, " E%d ", v);
        break;
    case 'A':
        e->n += snprintf(&e->log[e->n], room, "A%d ", v);
        break;
    default:
        e->n += snprintf(&e->log[e->n], room, " s%d ", v);
        break;
    }
}

static long s_ref_bytes = 0;
static long s_ref_ends = 0;
static long s_ref_aborts = 0;

static void ref_decode(const uint8_t *line, int nbits, Events *e)
{
    int ones = 8;
    bool active = false;
    uint8_t acc = 0;
    int nacc = 0;
    int count = 0;
    uint16_t crc = HDLC_CRC_INIT;
    for (int i = 0; i < nbits; i++)
    {
        int bit = (line[i / 8] >> (i % 8)) & 1;
        if (bit != 0)
        {
            if (active && ones == 6)
            {
                if (count >= HDLC_MIN_FRAME_BYTES)
                {
                    ev(e, 'A', count);
                    s_ref_aborts++;
                }
                else if (count > 0)
                {
                    ev(e, 's', count);
                }
                active = false;
                nacc = 0;
                count = 0;
                crc = HDLC_CRC_INIT;
            }
            else if (active)
            {
                acc = (uint8_t)((acc >> 1u) | 0x80u);
                if (++nacc == 8)
                {
                    ev(e, 'x', acc);
                    s_ref_bytes++;
                    crc = hdlc_crc_byte(crc, acc);
                    count++;
                    nacc = 0;
                }
            }
            ones = (ones < 8) ? ones + 1 : 8;
        }
        else
        {
            if (ones == 6)
            {
                if (active && count >= HDLC_MIN_FRAME_BYTES)
                {
                    int st = (nacc != 7) ? (int)HDLC_RX_ODD_BITS
                             : (crc != HDLC_CRC_GOOD) ? (int)HDLC_RX_BAD_CRC : (int)HDLC_RX_OK;
                    ev(e, 'E', st);
                    s_ref_ends++;
                }
                else if (active && count > 0)
                {
                    ev(e, 's', count);
                }
                active = true;
                nacc = 0;
                count = 0;
                crc = HDLC_CRC_INIT;
            }
            else if (active && ones != 5)
            {
                acc = (uint8_t)(acc >> 1u);
                if (++nacc == 8)
                {
                    ev(e, 'x', acc);
                    s_ref_bytes++;
                    crc = hdlc_crc_byte(crc, acc);
                    count++;
                    nacc = 0;
                }
            }
            ones = 0;
        }
    }
}

typedef struct
{
    Events *e;
    int count;
} LibCtx;

static void lib_byte(void *ctx, uint8_t data)
{
    LibCtx *c = (LibCtx *)ctx;
    ev(c->e, 'x', data);
    c->count++;
}

static void lib_end(void *ctx, hdlc_rx_status status)
{
    LibCtx *c = (LibCtx *)ctx;
    ev(c->e, 'E', (int)status);
    c->count = 0;
}

static void lib_abort(void *ctx)
{
    LibCtx *c = (LibCtx *)ctx;
    ev(c->e, 'A', c->count);
    c->count = 0;
}

static void lib_discard(void *ctx)
{
    LibCtx *c = (LibCtx *)ctx;
    ev(c->e, 's', c->count);
    c->count = 0;
}

static void fast_path_matches_reference(void)
{
    static Events ref;
    static Events lib;
    static uint8_t line[2048];
    uint32_t seed = 777u;
    int mismatches = 0;

    for (int round = 0; round < 2000; round++)
    {
        int pos = 0;
        int ones = 0;
        LibCtx ctx;
        hdlc_frame_sink sink;
        hdlc_decoder dec;
        memset(line, 0xFF, sizeof line);
        /* Random mix: flags, aborts, raw bit noise, and stuffed data runs. */
        while (pos < (int)sizeof line * 8 - 400)
        {
            uint32_t r;
            seed = seed * 1103515245u + 12345u;
            r = seed >> 16u;
            switch (r % 8u)
            {
            case 0:
            case 1:
                bits_put(line, &pos, 0x7Eu, 8);
                ones = 0;
                break;
            case 2:
                bits_put(line, &pos, 0x7Fu, 8);
                ones = 0;
                break;
            case 3:
                bits_put(line, &pos, r >> 3u, (int)(r % 7u));
                ones = 0;
                break;
            default:
            {
                int n = 1 + (int)((r >> 4u) % 40u);
                for (int i = 0; i < n; i++)
                {
                    uint32_t v;
                    seed = seed * 1103515245u + 12345u;
                    /* Leans towards ones so runs of 4..6 and stuffing are common. */
                    v = (seed >> 16u) | (seed >> 9u);
                    bits_put_stuffed(line, &pos, &ones, v & 0xFFu, 8);
                }
                break;
            }
            }
        }
        ref.n = 0;
        lib.n = 0;
        ref_decode(line, (int)sizeof line * 8, &ref);
        ctx.e = &lib;
        ctx.count = 0;
        sink.ctx = &ctx;
        sink.on_byte = lib_byte;
        sink.on_end = lib_end;
        sink.on_abort = lib_abort;
        sink.on_discard = lib_discard;
        hdlc_decoder_reset(&dec);
        for (int i = 0; i < (int)sizeof line; i++)
        {
            hdlc_decoder_feed(&dec, line[i], &sink);
        }
        if (ref.n != lib.n || memcmp(ref.log, lib.log, (size_t)ref.n) != 0)
        {
            if (mismatches == 0)
            {
                int k = 0;
                while (k < ref.n && k < lib.n && ref.log[k] == lib.log[k])
                {
                    k++;
                }
                printf("  round %d differs at %d\n  ref: %.120s\n  lib: %.120s\n", round, k,
                       &ref.log[k > 40 ? k - 40 : 0], &lib.log[k > 40 ? k - 40 : 0]);
            }
            mismatches++;
        }
    }
    CHECK_EQ(mismatches, 0);
    CHECK(s_ref_bytes > 100000);
    CHECK(s_ref_ends > 1000);
    CHECK(s_ref_aborts > 0);
}

/* ------------------------------------------------ bit-group encoder */

/** Append n bits (bit 0 first) to a bit line that starts all ones. */
static void group_put(uint8_t *line, int *pos, uint32_t v, int n)
{
    bits_put(line, pos, v, n);
}

static void bit_groups_equal_byte_form(void)
{
    uint32_t seed = 4242u;
    for (int round = 0; round < 300; round++)
    {
        static uint8_t payload[64];
        static uint8_t gl[256];
        Line bl;
        int gpos = 0;
        int len = 1 + (int)(seed % 64u);
        hdlc_encoder enc;
        uint32_t bits;
        int n;
        uint16_t fcs;
        for (int i = 0; i < len; i++)
        {
            seed = seed * 1103515245u + 12345u;
            payload[i] = (uint8_t)((seed >> 16u) | (seed >> 20u));   /* many ones */
        }
        /* Byte form. */
        bl.n = 0;
        encode_frame(&bl, payload, len);
        /* Bit-group form. */
        memset(gl, 0xFF, sizeof gl);
        n = hdlc_encoder_open_bits(&enc, &bits);
        CHECK_EQ(n, 8);
        group_put(gl, &gpos, bits, n);
        for (int i = 0; i < len; i++)
        {
            n = hdlc_encoder_write_bits(&enc, payload[i], &bits);
            CHECK(n >= 8 && n <= 10);
            group_put(gl, &gpos, bits, n);
        }
        fcs = hdlc_encoder_fcs(&enc);
        n = hdlc_encoder_write_bits(&enc, (uint8_t)(fcs & 0xFFu), &bits);
        group_put(gl, &gpos, bits, n);
        n = hdlc_encoder_write_bits(&enc, (uint8_t)(fcs >> 8u), &bits);
        group_put(gl, &gpos, bits, n);
        n = hdlc_encoder_close_bits(&enc, &bits);
        CHECK(n == 8 || n == 9);
        group_put(gl, &gpos, bits, n);
        /* The byte form pads its last byte with idle ones, and so does a line that starts all
         * ones, so the two must agree bit for bit over the byte form's whole length. */
        CHECK_EQ((gpos + 7) / 8, bl.n);
        CHECK(memcmp(gl, bl.b, (size_t)bl.n) == 0);
    }
}

int main(void)
{
    bit_groups_equal_byte_form();
    run_table_matches_rule();
    odd_bits_reported();
    too_long_cut_and_counted();
    fast_path_matches_reference();
    idle_then_six_ones_is_not_a_flag();
    crc_vectors();
    round_trip_every_offset();
    long_random_frames();
    back_to_back_frames_no_phantoms();
    corrupted_fcs_is_bad();
    abort_drops_frame();
    control_field();
    TEST_DONE();
}
