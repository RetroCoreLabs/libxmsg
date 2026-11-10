/*
 * term_keyseq.c - gather a function key's whole escape sequence (the rules are in
 * term_keyseq.h).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/term/term_keyseq.h"

#include <string.h>

#define KS_ESC 0x1Bu

/* What a byte does to the sequence held so far. */
typedef enum KsStep
{
    KS_MORE = 0, /* part of it; more to come */
    KS_END,      /* its final byte */
    KS_BREAK     /* cannot be part of it */
} KsStep;

static KsStep ks_step(const TermKeyseq *k, uint8_t b)
{
    uint8_t second;
    if (b < 0x20u || b > 0x7Eu)
    {
        return KS_BREAK;
    }
    if (k->held_len == 1u)
    {
        if (b == (uint8_t)'[' || b == (uint8_t)'O' || b <= 0x2Fu)
        {
            return KS_MORE; /* CSI, SS3, or an intermediate */
        }
        return KS_END; /* ESC + one byte 0x30..0x7E */
    }
    second = k->held[1];
    if (second == (uint8_t)'[')
    {
        /* parameters 0x30..0x3F and intermediates 0x20..0x2F go on; 0x40..0x7E ends */
        return (b >= 0x40u) ? KS_END : KS_MORE;
    }
    if (second == (uint8_t)'O')
    {
        return (b >= 0x40u) ? KS_END : KS_BREAK; /* SS3 takes exactly one byte */
    }
    return (b <= 0x2Fu) ? KS_MORE : KS_END; /* more intermediates, or the final byte */
}

/* Move what is held into out->unit. */
static void ks_release(TermKeyseq *k, TermKeyOut *out, bool whole)
{
    memcpy(out->unit, k->held, k->held_len);
    out->unit_len = k->held_len;
    out->whole = whole;
    k->held_len = 0;
}

/* The waits, checked before each byte and on each poll. */
static void ks_waits(TermKeyseq *k, uint32_t now_ms, TermKeyOut *out)
{
    uint32_t waited = (uint32_t)(now_ms - k->last_ms);
    if (k->held_len == 0u)
    {
        return;
    }
    if (k->held_len == 1u && k->esc_ms != 0u)
    {
        if (waited >= k->esc_ms)
        {
            k->held_len = 0;
            out->escape = true; /* nothing came after the ESC: the escape key */
        }
        return;
    }
    if (waited >= k->seq_ms)
    {
        ks_release(k, out, false); /* never finished: on as data */
    }
}

void term_keyseq_init(TermKeyseq *k, uint32_t esc_ms, uint32_t seq_ms)
{
    if (k == NULL)
    {
        return;
    }
    memset(k, 0, sizeof *k);
    k->esc_ms = esc_ms;
    k->seq_ms = (seq_ms == 0u) ? 1u : seq_ms;
}

int term_keyseq_byte(TermKeyseq *k, uint8_t b, uint32_t now_ms, TermKeyOut *out)
{
    if (k == NULL || out == NULL)
    {
        return -1;
    }
    memset(out, 0, sizeof *out);
    ks_waits(k, now_ms, out);
    if (k->held_len != 0u)
    {
        KsStep st = ks_step(k, b);
        if (st != KS_BREAK)
        {
            k->held[k->held_len++] = b;
            k->last_ms = now_ms;
            if (st == KS_END)
            {
                ks_release(k, out, true);
            }
            else if (k->held_len >= TERM_KEYSEQ_MAX)
            {
                ks_release(k, out, false); /* too long to be a key */
            }
            return 0;
        }
        ks_release(k, out, false); /* held bytes on as data, then this byte on its own */
    }
    /* Nothing is held now; out->unit may carry what was released just above. */
    if (b == KS_ESC)
    {
        k->held[0] = b;
        k->held_len = 1;
        k->last_ms = now_ms;
        return 0;
    }
    out->has_byte = true;
    out->byte = b;
    return 0;
}

bool term_keyseq_poll(TermKeyseq *k, uint32_t now_ms, TermKeyOut *out)
{
    if (k == NULL || out == NULL)
    {
        return false;
    }
    memset(out, 0, sizeof *out);
    ks_waits(k, now_ms, out);
    return out->escape || out->unit_len != 0u;
}

bool term_keyseq_holding(const TermKeyseq *k)
{
    return k != NULL && k->held_len != 0u;
}
