/*
 * xrout_letter.c - XROUT letters and replies.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/xrout/xrout_letter.h"

#include <string.h>

#include "libxmsg/gen/xrout_services.h"
#include "libxmsg/xmsg_endian.h"

void xrout_head_write(uint8_t *out, uint8_t serial, uint8_t service, uint16_t payload_len)
{
    out[0] = serial;
    out[1] = service;
    xmsg_be16_write(out + 2, payload_len);
}

/* Append one string parameter: fill to even, negative type byte, length, bytes. */
static size_t put_string_param(uint8_t *out, size_t cap, size_t pos, uint8_t number,
                               const char *text)
{
    size_t len = strlen(text);
    if (len > 255)
    {
        return 0;
    }
    if ((pos & 1u) != 0)
    {
        if (pos >= cap)
        {
            return 0;
        }
        out[pos++] = 0x00;
    }
    if (pos + 2 + len > cap)
    {
        return 0;
    }
    out[pos++] = (uint8_t)(0x100u - number); /* -number as a byte */
    out[pos++] = (uint8_t)len;
    memcpy(out + pos, text, len);
    return pos + len;
}

size_t xrout_letter_build(uint8_t *out, size_t cap, const char *service, const char *system)
{
    size_t pos;
    if (out == NULL || service == NULL || system == NULL || cap < XROUT_HEAD_SIZE)
    {
        return 0;
    }
    pos = XROUT_HEAD_SIZE;
    pos = put_string_param(out, cap, pos, 1, service);
    if (pos == 0)
    {
        return 0;
    }
    pos = put_string_param(out, cap, pos, 2, system);
    if (pos == 0)
    {
        return 0;
    }
    xrout_head_write(out, 0, (uint8_t)XROUT_SERVICE_XSLET, (uint16_t)(pos - XROUT_HEAD_SIZE));
    return pos;
}

int xrout_parse(const uint8_t *body, size_t len, XroutMessage *m)
{
    size_t pos;
    size_t end;

    if (body == NULL || m == NULL || len < XROUT_HEAD_SIZE)
    {
        return XMSG_EINVAL;
    }
    memset(m, 0, sizeof *m);
    m->serial = body[0];
    m->service = body[1];
    m->length = xmsg_be16_read(body + 2);
    /* The declared length bounds the walk, clamped to what is present. */
    end = XROUT_HEAD_SIZE + m->length;
    if (end > len)
    {
        end = len;
    }
    pos = XROUT_HEAD_SIZE;
    while (pos < end)
    {
        uint8_t type = body[pos];
        if (type == 0)
        {
            pos++; /* fill byte keeping the next parameter on an even offset */
            continue;
        }
        if (pos + 2 > end)
        {
            return XMSG_EPROTO;
        }
        uint8_t plen = body[pos + 1];
        if (pos + 2 + plen > end)
        {
            return XMSG_EPROTO;
        }
        if (m->param_count < XROUT_MAX_PARAMS)
        {
            XroutParam *p = &m->params[m->param_count++];
            p->is_string = (type & 0x80u) != 0;
            p->number = p->is_string ? (uint8_t)(0x100u - type) : type;
            p->data = body + pos + 2;
            p->len = plen;
        }
        pos += 2 + plen;
    }
    return XMSG_OK;
}

int32_t xrout_param_int(const XroutMessage *m, uint8_t number)
{
    for (int i = 0; i < m->param_count; i++)
    {
        const XroutParam *p = &m->params[i];
        if (p->number != number || p->is_string)
        {
            continue;
        }
        if (p->len == 1)
        {
            return p->data[0];
        }
        if (p->len == 2)
        {
            return xmsg_be16_read(p->data);
        }
        if (p->len == 4)
        {
            return (int32_t)(xmsg_be32_read(p->data) & 0x7FFFFFFFu);
        }
        return -1;
    }
    return -1;
}

const char *xrout_error_name(uint8_t code)
{
    switch (code)
    {
    case XROUT_ERR_XRSOK:
        return "ok";
    case XROUT_ERR_XRUNN:
        return "no such name: the service is not running there, or the name is spelled differently";
    case XROUT_ERR_XRNSP:
        return "the service has no free seat";
    case XROUT_ERR_XRUNM:
        return "that address means nothing there: the port has closed";
    case XROUT_ERR_XRNCO:
        return "no connection to work with";
    case XROUT_ERR_XRNRO:
        return "no access to the remote system: we are not one of its friend systems";
    default:
        return "XROUT error";
    }
}
