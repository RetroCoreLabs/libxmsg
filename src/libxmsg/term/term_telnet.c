/*
 * term_telnet.c - the telnet part of a command line on TCP: option talk in, IAC doubling out.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * The rules were in examples/host_tad_telnet/host_tad_telnet.c (tn_option, tn_open, client_rx
 * and the CR NUL rule of session_byte) and are moved here unchanged.
 */
#include "libxmsg/term/term_telnet.h"

#include <string.h>

static bool bit_get(const uint8_t *set, uint8_t opt)
{
    return (set[opt >> 3u] & (uint8_t)(1u << (opt & 0x07u))) != 0u;
}

static void bit_put(uint8_t *set, uint8_t opt, bool on)
{
    uint8_t mask = (uint8_t)(1u << (opt & 0x07u));
    if (on)
    {
        set[opt >> 3u] = (uint8_t)(set[opt >> 3u] | mask);
    }
    else
    {
        set[opt >> 3u] = (uint8_t)(set[opt >> 3u] & (uint8_t)~mask);
    }
}

void term_telnet_init(TermTelnet *t, bool binary)
{
    if (t == NULL)
    {
        return;
    }
    memset(t, 0, sizeof *t);
    t->binary = binary;
}

/* The options we do ourselves, and the ones we let the client do. */
static bool we_do(const TermTelnet *t, uint8_t opt)
{
    return opt == TERM_TN_ECHO || opt == TERM_TN_SGA || (t->binary && opt == TERM_TN_BINARY);
}

static bool client_may(const TermTelnet *t, uint8_t opt)
{
    return opt == TERM_TN_SGA || (t->binary && opt == TERM_TN_BINARY);
}

static size_t put3(uint8_t *out, uint8_t verb, uint8_t opt)
{
    out[0] = TERM_TN_IAC;
    out[1] = verb;
    out[2] = opt;
    return 3u;
}

int term_telnet_open(TermTelnet *t, uint8_t *out, size_t cap)
{
    static const uint8_t mine[] = {TERM_TN_ECHO, TERM_TN_SGA, TERM_TN_BINARY};
    static const uint8_t theirs[] = {TERM_TN_SGA, TERM_TN_BINARY};
    size_t n = 0;
    size_t mine_n;
    size_t theirs_n;

    if (t == NULL || out == NULL)
    {
        return -1;
    }
    mine_n = t->binary ? 3u : 2u;
    theirs_n = t->binary ? 2u : 1u;
    if (cap < (3u * (mine_n + theirs_n)))
    {
        return -1;
    }
    for (size_t i = 0; i < mine_n; i++)
    {
        bit_put(t->us_on, mine[i], true);
        n += put3(&out[n], TERM_TN_WILL, mine[i]);
    }
    for (size_t i = 0; i < theirs_n; i++)
    {
        bit_put(t->him_on, theirs[i], true);
        n += put3(&out[n], TERM_TN_DO, theirs[i]);
    }
    return (int)n;
}

/* Option negotiation (RFC 854, with the loop rule of RFC 1143: answer only a CHANGE). */
static size_t option(TermTelnet *t, uint8_t verb, uint8_t opt, uint8_t *reply)
{
    switch (verb)
    {
    case TERM_TN_DO:
        if (!we_do(t, opt))
        {
            return put3(reply, TERM_TN_WONT, opt);
        }
        if (!bit_get(t->us_on, opt))
        {
            bit_put(t->us_on, opt, true);
            return put3(reply, TERM_TN_WILL, opt);
        }
        return 0u;
    case TERM_TN_DONT:
        if (bit_get(t->us_on, opt))
        {
            bit_put(t->us_on, opt, false);
            return put3(reply, TERM_TN_WONT, opt);
        }
        return 0u;
    case TERM_TN_WILL:
        if (!client_may(t, opt))
        {
            return put3(reply, TERM_TN_DONT, opt);
        }
        if (!bit_get(t->him_on, opt))
        {
            bit_put(t->him_on, opt, true);
            return put3(reply, TERM_TN_DO, opt);
        }
        return 0u;
    default: /* TERM_TN_WONT */
        if (bit_get(t->him_on, opt))
        {
            bit_put(t->him_on, opt, false);
            return put3(reply, TERM_TN_DONT, opt);
        }
        return 0u;
    }
}

/* A data byte: the CR NUL rule outside binary mode, then the byte as it is. */
static int data(TermTelnet *t, uint8_t b)
{
    if (t->after_cr && b == 0x00u && !bit_get(t->him_on, TERM_TN_BINARY))
    {
        t->after_cr = false;
        return -1;
    }
    t->after_cr = (b == 0x0Du);
    return (int)b;
}

int term_telnet_rx(TermTelnet *t, uint8_t b, uint8_t reply[TERM_TELNET_REPLY_MAX],
                   size_t *reply_len)
{
    if (t == NULL || reply == NULL || reply_len == NULL)
    {
        return -2;
    }
    *reply_len = 0;
    switch (t->rx)
    {
    case TERM_TN_RX_DATA:
        if (b == TERM_TN_IAC)
        {
            t->rx = TERM_TN_RX_IAC;
            return -1;
        }
        return data(t, b);
    case TERM_TN_RX_IAC:
        t->rx = TERM_TN_RX_DATA;
        if (b == TERM_TN_IAC)
        {
            return data(t, b); /* IAC IAC is the data byte 0xFF */
        }
        if (b == TERM_TN_SB)
        {
            t->rx = TERM_TN_RX_SB;
        }
        else if (b >= TERM_TN_WILL)
        {
            t->verb = b; /* WILL, WONT, DO, DONT: one option byte follows */
            t->rx = TERM_TN_RX_VERB;
        }
        return -1; /* NOP, GA, AYT and the rest: ignored */
    case TERM_TN_RX_VERB:
        t->rx = TERM_TN_RX_DATA;
        *reply_len = option(t, t->verb, b, reply);
        return -1;
    case TERM_TN_RX_SB:
        t->rx = (b == TERM_TN_IAC) ? TERM_TN_RX_SB_IAC : TERM_TN_RX_SB;
        return -1;
    default: /* TERM_TN_RX_SB_IAC */
        t->rx = (b == TERM_TN_SE) ? TERM_TN_RX_DATA : TERM_TN_RX_SB;
        return -1;
    }
}

int term_telnet_escape(const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *taken)
{
    size_t o = 0;
    size_t i = 0;

    if ((in == NULL && n != 0u) || out == NULL || taken == NULL)
    {
        return -1;
    }
    for (i = 0; i < n; i++)
    {
        size_t need = (in[i] == TERM_TN_IAC) ? 2u : 1u;
        if (o + need > cap)
        {
            break;
        }
        if (in[i] == TERM_TN_IAC)
        {
            out[o++] = TERM_TN_IAC;
        }
        out[o++] = in[i];
    }
    *taken = i;
    return (int)o;
}

bool term_telnet_is_on(const TermTelnet *t, uint8_t opt, bool client)
{
    if (t == NULL)
    {
        return false;
    }
    return bit_get(client ? t->him_on : t->us_on, opt);
}
