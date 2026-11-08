/*
 * sintran_hdr.c - the SINTRAN datagram header and its ones-complement checksum.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/sintran/sintran_hdr.h"

#include "libxmsg/xmsg_endian.h"

uint16_t sintran_hdr_checksum(const uint8_t *hdr14)
{
    /* The kernel loop (carved at 137314): RADD with the carry folded back in after every word
     * (end-around carry), then the complement - skipped when the sum is exactly zero. */
    uint32_t sum = 0;
    for (int w = 0; w < 6; w++)
    {
        sum += xmsg_be16_read(hdr14 + 2 * w);
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    if (sum == 0)
    {
        return 0;
    }
    return (uint16_t)(~sum & 0xFFFFu);
}

void sintran_hdr_fill(SintranHeader *h, uint8_t subtype, uint16_t dst_node, uint16_t src_node,
                      uint16_t flags1, uint16_t flags2)
{
    h->version = (uint8_t)SINTRAN_VERSION_BYTE;
    h->hop = (uint8_t)SINTRAN_HOP_DIRECT;
    h->type = 0;
    h->subtype = subtype;
    h->dst_node = dst_node;
    h->src_node = src_node;
    h->flags1 = flags1;
    h->flags2 = flags2;
    h->checksum = 0;
}

void sintran_hdr_write(uint8_t *out, SintranHeader *h)
{
    out[0] = h->version;
    out[1] = h->hop;
    out[2] = h->type;
    out[3] = h->subtype;
    xmsg_be16_write(out + 4, h->dst_node);
    xmsg_be16_write(out + 6, h->src_node);
    xmsg_be16_write(out + 8, h->flags1);
    xmsg_be16_write(out + 10, h->flags2);
    h->checksum = sintran_hdr_checksum(out);
    xmsg_be16_write(out + 12, h->checksum);
}

int sintran_hdr_parse(const uint8_t *in, size_t len, SintranHeader *h)
{
    if (in == NULL || h == NULL || len < SINTRAN_HDR_SIZE)
    {
        return XMSG_EINVAL;
    }
    h->version = in[0];
    h->hop = in[1];
    h->type = in[2];
    h->subtype = in[3];
    h->dst_node = xmsg_be16_read(in + 4);
    h->src_node = xmsg_be16_read(in + 6);
    h->flags1 = xmsg_be16_read(in + 8);
    h->flags2 = xmsg_be16_read(in + 10);
    h->checksum = xmsg_be16_read(in + 12);
    if (h->version != (uint8_t)SINTRAN_VERSION_BYTE || sintran_hdr_checksum(in) != h->checksum)
    {
        return XMSG_EPROTO;
    }
    return XMSG_OK;
}
