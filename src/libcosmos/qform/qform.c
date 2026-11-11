/*
 * qform.c - QFORM reader and writer.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libcosmos/qform/qform.h"

#include <string.h>

/* The marker byte in an escaped length: the real length is the byte after it. Pinned by the
 * captured listing request 8C 80 46 (RetroIO QformReader.LengthEscapeMarker). */
#define QFORM_LENGTH_MARKER 0x80u

void qform_reader_init(QformReader *r, const uint8_t *buf, size_t len)
{
    if (r == NULL)
    {
        return;
    }
    memset(r, 0, sizeof *r);
    r->buf = buf;
    r->len = (buf != NULL) ? len : 0u;
    r->ends[0] = r->len;
}

/* Read an escaped length at r->pos (below `end`). Returns false when it runs past. */
static bool read_escaped_length(QformReader *r, size_t end, size_t *out)
{
    uint8_t first;

    if (r->pos >= end)
    {
        return false;
    }
    first = r->buf[r->pos++];
    if (first != QFORM_LENGTH_MARKER)
    {
        *out = first;
        return true;
    }
    if (r->pos >= end)
    {
        return false;
    }
    *out = r->buf[r->pos++];
    return true;
}

bool qform_reader_next(QformReader *r, QformItem *it)
{
    if (r == NULL || it == NULL || r->failed)
    {
        return false;
    }
    for (;;)
    {
        size_t end = r->ends[r->depth];
        size_t start = r->pos;
        uint8_t tag;
        size_t len;
        uint8_t cls;

        /* The end of this level, a byte with bit 7 clear, or a lone last byte (padding) all end
         * the level; the walk then carries on after the constructed value that held it. */
        if (r->pos >= end || (r->buf[r->pos] & 0x80u) == 0u || r->pos == end - 1u)
        {
            if (r->depth == 0u)
            {
                r->pos = r->len;
                return false;
            }
            r->pos = end;
            r->depth--;
            continue;
        }
        tag = r->buf[r->pos++];
        cls = (uint8_t)((tag & 0x70u) >> 4u);
        len = (size_t)(tag & 0x0Fu);
        if (cls == QFORM_CLASS_CONSTRUCTED || len == 0u)
        {
            if (!read_escaped_length(r, end, &len))
            {
                r->failed = true;
                return false;
            }
        }
        if (len > end - r->pos)
        {
            r->failed = true;
            return false;
        }
        it->tag = tag;
        it->cls = cls;
        it->depth = r->depth;
        it->value = r->buf + r->pos;
        it->len = len;
        it->offset = start;
        if (cls == QFORM_CLASS_CONSTRUCTED && len > 0u)
        {
            if (r->depth >= QFORM_MAX_DEPTH)
            {
                r->failed = true;
                return false;
            }
            r->depth++;
            r->ends[r->depth] = r->pos + len; /* descend: the contents come next */
        }
        else
        {
            r->pos += len;
        }
        return true;
    }
}

bool qform_find_field(const uint8_t *buf, size_t len, uint16_t selector, QformItem *it)
{
    QformReader r;
    QformItem cur;
    bool want_next = false;

    if (it == NULL)
    {
        return false;
    }
    qform_reader_init(&r, buf, len);
    while (qform_reader_next(&r, &cur))
    {
        if (cur.depth != 0u)
        {
            continue;
        }
        if (want_next)
        {
            *it = cur;
            return true;
        }
        if (cur.tag == QFORM_TAG_SELECTOR && cur.len == 2u &&
            (uint16_t)((cur.value[0] << 8u) | cur.value[1]) == selector)
        {
            want_next = true;
        }
    }
    return false;
}

bool qform_item_uint(const QformItem *it, uint32_t *out)
{
    uint32_t v = 0;

    if (it == NULL || out == NULL || it->cls == QFORM_CLASS_CONSTRUCTED || it->len < 1u ||
        it->len > 4u)
    {
        return false;
    }
    for (size_t i = 0; i < it->len; i++)
    {
        v = (v << 8u) | it->value[i];
    }
    *out = v;
    return true;
}

void qform_writer_init(QformWriter *w, uint8_t *buf, size_t cap)
{
    if (w == NULL)
    {
        return;
    }
    w->buf = buf;
    w->cap = (buf != NULL) ? cap : 0u;
    w->len = 0;
    w->failed = false;
}

void qform_put_raw(QformWriter *w, const uint8_t *bytes, size_t n)
{
    if (w == NULL || w->failed)
    {
        return;
    }
    if (n > w->cap - w->len)
    {
        w->failed = true;
        return;
    }
    if (n > 0u)
    {
        memcpy(w->buf + w->len, bytes, n);
    }
    w->len += n;
}

void qform_put_u8(QformWriter *w, uint8_t v)
{
    qform_put_raw(w, &v, 1u);
}

void qform_put_tag16(QformWriter *w, uint8_t tag, uint16_t v)
{
    const uint8_t b[3] = {tag, (uint8_t)(v >> 8u), (uint8_t)(v & 0xFFu)};
    qform_put_raw(w, b, sizeof b);
}

void qform_put_typed32(QformWriter *w, uint32_t v)
{
    const uint8_t b[5] = {QFORM_TAG_TYPED32, (uint8_t)(v >> 24u), (uint8_t)((v >> 16u) & 0xFFu),
                          (uint8_t)((v >> 8u) & 0xFFu), (uint8_t)(v & 0xFFu)};
    qform_put_raw(w, b, sizeof b);
}

void qform_put_string(QformWriter *w, const uint8_t *bytes, size_t n)
{
    if (w == NULL)
    {
        return;
    }
    /* MEASURED: Bx for 13 bytes (the reserve's asker), B0 10 for a 16-byte open name and B0 12
     * for an 18-byte one (capture-read.txt, capture-open-error.txt). A length of 0x80 would
     * collide with the escape marker and no capture shows how a client writes it, so it is
     * refused rather than guessed. */
    if (n == 0u || n > 255u || n == QFORM_LENGTH_MARKER)
    {
        w->failed = true;
        return;
    }
    if (n <= 15u)
    {
        qform_put_u8(w, (uint8_t)(QFORM_TAG_STRING | n));
    }
    else
    {
        qform_put_u8(w, QFORM_TAG_STRING);
        qform_put_u8(w, (uint8_t)n);
    }
    qform_put_raw(w, bytes, n);
}

void qform_put_selector(QformWriter *w, uint16_t selector)
{
    qform_put_tag16(w, QFORM_TAG_SELECTOR, selector);
}

size_t qform_begin_constructed(QformWriter *w)
{
    size_t at;

    if (w == NULL)
    {
        return 0;
    }
    at = w->len;
    qform_put_u8(w, QFORM_TAG_CONSTRUCTED);
    qform_put_u8(w, 0);
    return at;
}

void qform_end_constructed(QformWriter *w, size_t at)
{
    size_t content;

    if (w == NULL || w->failed)
    {
        return;
    }
    content = w->len - at - 2u;
    if (content >= QFORM_LENGTH_MARKER)
    {
        w->failed = true;
        return;
    }
    w->buf[at + 1u] = (uint8_t)content;
}
