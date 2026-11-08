/*
 * xmsg_subhdr.c - the XMSG sub-header.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/xmsg/xmsg_subhdr.h"

#include "libxmsg/xmsg_endian.h"

void xmsg_subhdr_write(uint8_t *out, const XmsgSubhdr *s)
{
    out[0] = (uint8_t)XMSG_SUBHDR_MARKER0;
    out[1] = (uint8_t)XMSG_SUBHDR_MARKER1;
    out[2] = s->frame_flags;
    out[3] = s->role;
    xmsg_be16_write(out + 4, s->dst_system);
    xmsg_be16_write(out + 6, s->dst_port);
    xmsg_be16_write(out + 8, s->src_system);
    xmsg_be16_write(out + 10, s->src_port);
    xmsg_be16_write(out + 12, s->xmcsm);
}

int xmsg_subhdr_parse(const uint8_t *in, size_t len, XmsgSubhdr *s)
{
    if (in == NULL || s == NULL || len < XMSG_SUBHDR_SIZE)
    {
        return XMSG_EINVAL;
    }
    s->frame_flags = in[2];
    s->role = in[3];
    s->dst_system = xmsg_be16_read(in + 4);
    s->dst_port = xmsg_be16_read(in + 6);
    s->src_system = xmsg_be16_read(in + 8);
    s->src_port = xmsg_be16_read(in + 10);
    s->xmcsm = xmsg_be16_read(in + 12);
    if (in[0] != (uint8_t)XMSG_SUBHDR_MARKER0 || in[1] != (uint8_t)XMSG_SUBHDR_MARKER1)
    {
        return XMSG_EPROTO;
    }
    return XMSG_OK;
}
