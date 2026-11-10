/*
 * xrout_name.c - XSGNI name table and messages (see xrout_name.h).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/xrout/xrout_name.h"

#include <string.h>

#include "libxmsg/xmsg_endian.h"
#include "libxmsg/xrout/xrout_letter.h"

static void row_clear(XroutNameRow *r)
{
    memset(r, 0, sizeof *r);
    r->free = XROUT_NAME_NO_FREE;
}

static bool row_is_valid(const XroutNameRow *r)
{
    const char *nul;
    if (r == NULL || r->system == 0 || r->port == 0)
    {
        return false;
    }
    nul = memchr(r->name, 0, sizeof r->name);
    if (nul == NULL || nul == r->name)
    {
        return false; /* no end inside the array, or an empty name */
    }
    return r->free == XROUT_NAME_NO_FREE || (r->free >= 0 && r->free <= 0xFFFF);
}

void xrout_name_init(XroutNameTable *t)
{
    if (t != NULL)
    {
        memset(t, 0, sizeof *t);
    }
}

int xrout_name_row_set(XroutNameRow *r, uint16_t system, uint16_t port, const char *name,
                       int32_t free)
{
    size_t len;
    if (r == NULL || name == NULL)
    {
        return XMSG_EINVAL;
    }
    len = strlen(name);
    if (len == 0 || len > XROUT_NAME_LEN_MAX)
    {
        return XMSG_EINVAL;
    }
    row_clear(r);
    r->system = system;
    r->port = port;
    r->free = free;
    memcpy(r->name, name, len);
    return row_is_valid(r) ? XMSG_OK : XMSG_EINVAL;
}

int xrout_name_put(XroutNameTable *t, const XroutNameRow *r)
{
    uint8_t at;
    uint32_t magic;
    if (t == NULL || !row_is_valid(r))
    {
        return XMSG_EINVAL;
    }
    magic = xrout_name_magic(r);
    for (at = 0; at < t->count; at++)
    {
        const uint32_t here = xrout_name_magic(&t->r[at]);
        if (here == magic)
        {
            t->r[at] = *r;
            return XMSG_OK;
        }
        if (here > magic)
        {
            break;
        }
    }
    if (t->count >= XROUT_NAME_ROWS_MAX)
    {
        return XMSG_ENOSPC;
    }
    /* Ascending magic order: the lookup is then the first row at or above the asked magic. */
    memmove(&t->r[at + 1], &t->r[at], (size_t)(t->count - at) * sizeof t->r[0]);
    t->r[at] = *r;
    t->count++;
    return XMSG_OK;
}

int xrout_name_remove(XroutNameTable *t, uint16_t system, uint16_t port)
{
    if (t == NULL)
    {
        return XMSG_EINVAL;
    }
    for (uint8_t i = 0; i < t->count; i++)
    {
        if (t->r[i].system == system && t->r[i].port == port)
        {
            memmove(&t->r[i], &t->r[i + 1], (size_t)(t->count - i - 1u) * sizeof t->r[0]);
            t->count--;
            return XMSG_OK;
        }
    }
    return XMSG_EINVAL;
}

bool xrout_name_has(const XroutNameTable *t, uint16_t system, uint16_t port)
{
    if (t == NULL)
    {
        return false;
    }
    for (uint8_t i = 0; i < t->count; i++)
    {
        if (t->r[i].system == system && t->r[i].port == port)
        {
            return true;
        }
    }
    return false;
}

bool xrout_name_lookup(const XroutNameTable *t, uint32_t asked, XroutNameRow *out)
{
    row_clear(out);
    if (t == NULL)
    {
        return false;
    }
    for (uint8_t i = 0; i < t->count; i++)
    {
        if (xrout_name_magic(&t->r[i]) >= asked)
        {
            *out = t->r[i];
            return true;
        }
    }
    return false;
}

size_t xrout_name_request_write(uint8_t *out, uint32_t asked)
{
    if (asked == 0)
    {
        /* The captured first call: 01 45 00 04 01 02 00 00. */
        xrout_head_write(out, XROUT_NAME_SERIAL, XROUT_NAME_SERVICE, 4);
        out[4] = 0x01;
        out[5] = 0x02;
        xmsg_be16_write(out + 6, 0);
        return 8;
    }
    /* Every captured next call: 01 45 00 06 01 04 <magic32>. */
    xrout_head_write(out, XROUT_NAME_SERIAL, XROUT_NAME_SERVICE, 6);
    out[4] = 0x01;
    out[5] = 0x04;
    xmsg_be32_write(out + 6, asked);
    return 10;
}

/* Integer parameter `number` as an unsigned 32-bit value. xrout_param_int() masks a 4-byte value
 * to 31 bits; a magic for system 0x8000 and above needs all 32. */
static bool param_u32(const XroutMessage *m, uint8_t number, uint32_t *value)
{
    for (int i = 0; i < m->param_count; i++)
    {
        const XroutParam *p = &m->params[i];
        if (p->number != number || p->is_string)
        {
            continue;
        }
        switch (p->len)
        {
        case 1:
            *value = p->data[0];
            return true;
        case 2:
            *value = xmsg_be16_read(p->data);
            return true;
        case 4:
            *value = xmsg_be32_read(p->data);
            return true;
        default:
            return false;
        }
    }
    return false;
}

static const XroutParam *param_string(const XroutMessage *m, uint8_t number)
{
    for (int i = 0; i < m->param_count; i++)
    {
        if (m->params[i].number == number && m->params[i].is_string)
        {
            return &m->params[i];
        }
    }
    return NULL;
}

int xrout_name_request_read(const uint8_t *body, size_t len, uint8_t *serial, uint32_t *asked)
{
    XroutMessage m;
    int rc;
    if (body == NULL || asked == NULL)
    {
        return XMSG_EINVAL;
    }
    rc = xrout_parse(body, len, &m);
    if (rc != XMSG_OK)
    {
        return rc;
    }
    if (m.service != XROUT_NAME_SERVICE || !param_u32(&m, 1, asked))
    {
        return XMSG_EPROTO;
    }
    if (serial != NULL)
    {
        *serial = m.serial;
    }
    return XMSG_OK;
}

size_t xrout_name_reply_write(uint8_t *out, uint8_t serial, const XroutNameRow *r)
{
    size_t pos;
    size_t len;
    if (out == NULL)
    {
        return 0;
    }
    if (r == NULL)
    {
        /* The captured end of table: 01 00 00 04 01 02 00 00. */
        xrout_head_write(out, serial, 0x00, 4);
        out[4] = 0x01;
        out[5] = 0x02;
        xmsg_be16_write(out + 6, 0);
        return 8;
    }
    if (!row_is_valid(r))
    {
        return 0;
    }
    len = strlen(r->name);
    pos = XROUT_HEAD_SIZE;
    out[pos++] = 0x01;
    out[pos++] = 0x04;
    xmsg_be32_write(out + pos, xrout_name_magic(r));
    pos += 4;
    out[pos++] = 0xFE; /* -2: string parameter 2 */
    out[pos++] = (uint8_t)len;
    memcpy(out + pos, r->name, len);
    pos += len;
    if ((len & 1u) != 0)
    {
        /* Pad to even, counted in the length, also when it is the last byte (captured
         * *TADADM and *XM-ENNS0 replies end in 00). */
        out[pos++] = 0x00;
    }
    if (r->free != XROUT_NAME_NO_FREE)
    {
        out[pos++] = 0x03;
        out[pos++] = 0x02;
        xmsg_be16_write(out + pos, (uint16_t)r->free);
        pos += 2;
    }
    /* Status 0x00 = XRSOK; the serial is echoed (xrout_route.c says why). */
    xrout_head_write(out, serial, 0x00, (uint16_t)(pos - XROUT_HEAD_SIZE));
    return pos;
}

int xrout_name_reply_read(const uint8_t *body, size_t len, uint8_t *status, XroutNameRow *r,
                          bool *is_end)
{
    XroutMessage m;
    const XroutParam *name;
    uint32_t magic = 0;
    uint32_t free_seats = 0;
    int rc;
    if (body == NULL || r == NULL || is_end == NULL)
    {
        return XMSG_EINVAL;
    }
    row_clear(r);
    *is_end = false;
    rc = xrout_parse(body, len, &m);
    if (rc != XMSG_OK)
    {
        return rc;
    }
    if (status != NULL)
    {
        *status = m.service;
    }
    if (m.service != 0x00 || !param_u32(&m, 1, &magic))
    {
        return XMSG_EPROTO;
    }
    if (magic == 0)
    {
        *is_end = true;
        return XMSG_OK;
    }
    name = param_string(&m, 2);
    if (name == NULL || name->len == 0 || name->len > XROUT_NAME_LEN_MAX)
    {
        return XMSG_EPROTO;
    }
    r->system = (uint16_t)(magic >> 16u);
    r->port = (uint16_t)(magic & 0xFFFFu);
    memcpy(r->name, name->data, name->len);
    r->name[name->len] = '\0';
    if (param_u32(&m, 3, &free_seats))
    {
        r->free = (int32_t)(free_seats & 0xFFFFu);
    }
    return XMSG_OK;
}
