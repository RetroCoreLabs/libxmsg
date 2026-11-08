/*
 * tad_codec.c - the TAD message chain.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/tad/tad_codec.h"

#include <string.h>

#include "libxmsg/xmsg_endian.h"

void tad_reader_init(TadReader *r, const uint8_t *chain, size_t len)
{
    r->chain = chain;
    r->len = (chain == NULL) ? 0 : len;
    r->pos = 0;
}

bool tad_reader_next(TadReader *r, TadMessage *out)
{
    size_t avail;

    while (r->pos < r->len && r->chain[r->pos] == 0x00)
    {
        r->pos++; /* a lone 0x00 is the host's alignment fill, not a message */
    }
    if (r->pos + 2 > r->len)
    {
        r->pos = r->len; /* nothing left, or an opcode with no count byte at a cut */
        return false;
    }
    out->opcode = r->chain[r->pos];
    out->count = r->chain[r->pos + 1];
    out->data = r->chain + r->pos + 2;
    avail = r->len - (r->pos + 2);
    out->len = (uint8_t)((avail < out->count) ? avail : out->count);
    r->pos += 2u + out->count;
    if (r->pos > r->len)
    {
        r->pos = r->len;
    }
    return true;
}

bool tad_chain_has(const uint8_t *chain, size_t len, uint8_t opcode)
{
    TadReader r;
    TadMessage m;

    tad_reader_init(&r, chain, len);
    while (tad_reader_next(&r, &m))
    {
        if (m.opcode == opcode)
        {
            return true;
        }
    }
    return false;
}

void tad_writer_init(TadWriter *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = (buf == NULL) ? 0 : cap;
    w->len = 0;
    w->overflow = false;
}

int tad_writer_add(TadWriter *w, uint8_t opcode, const uint8_t *data, size_t len)
{
    if (len > TAD_MAX_DATA || (len > 0 && data == NULL))
    {
        return XMSG_EINVAL;
    }
    if (w->overflow)
    {
        return XMSG_ENOSPC;
    }
    if (w->len + 2u + len > w->cap)
    {
        w->overflow = true;
        return XMSG_ENOSPC;
    }
    w->buf[w->len++] = opcode;
    w->buf[w->len++] = (uint8_t)len;
    if (len > 0)
    {
        memcpy(w->buf + w->len, data, len);
        w->len += len;
    }
    return XMSG_OK;
}

size_t tad_writer_length(const TadWriter *w)
{
    return w->overflow ? 0 : w->len;
}

uint8_t tad_even_parity(uint8_t c)
{
    uint8_t v = (uint8_t)(c & 0x7Fu);
    uint8_t ones = 0;

    for (int i = 0; i < 7; i++)
    {
        ones = (uint8_t)(ones + ((v >> i) & 1u));
    }
    return (uint8_t)(((ones & 1u) != 0) ? (v | 0x80u) : v);
}

void tad_body_head_write(uint8_t *out, uint16_t lead, uint16_t chain_len)
{
    xmsg_be16_write(out, lead);
    xmsg_be16_write(out + 2, chain_len);
}

int tad_body_split(const uint8_t *body, size_t len, uint16_t *lead, const uint8_t **chain,
                   size_t *clen)
{
    size_t declared;

    if (body == NULL || lead == NULL || chain == NULL || clen == NULL || len < TAD_BODY_HEAD_SIZE)
    {
        return XMSG_EINVAL;
    }
    *lead = xmsg_be16_read(body);
    declared = xmsg_be16_read(body + 2);
    *chain = body + TAD_BODY_HEAD_SIZE;
    *clen = (declared < len - TAD_BODY_HEAD_SIZE) ? declared : len - TAD_BODY_HEAD_SIZE;
    return XMSG_OK;
}
