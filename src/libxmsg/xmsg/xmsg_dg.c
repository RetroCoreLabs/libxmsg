/*
 * xmsg_dg.c - datagram parse and build.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/xmsg/xmsg_dg.h"

#include <string.h>

int xmsg_dg_parse(const uint8_t *info, size_t len, XmsgDatagram *dg)
{
    int rc;

    if (info == NULL || dg == NULL)
    {
        return XMSG_EINVAL;
    }
    memset(dg, 0, sizeof *dg);
    rc = sintran_hdr_parse(info, len, &dg->hdr);
    if (rc != XMSG_OK)
    {
        return rc;
    }
    dg->has_subhdr = false;
    dg->body = info + SINTRAN_HDR_SIZE;
    dg->body_len = len - SINTRAN_HDR_SIZE;

    if (dg->hdr.subtype == SINTRAN_SUBTYPE_DATA || dg->hdr.subtype == SINTRAN_SUBTYPE_FIRST_FRAG)
    {
        /* A data frame without room for its sub-header, or with a wrong marker, is not one. */
        if (len < XMSG_DG_HDR_SIZE)
        {
            return XMSG_EPROTO;
        }
        rc = xmsg_subhdr_parse(info + SINTRAN_HDR_SIZE, len - SINTRAN_HDR_SIZE, &dg->sub);
        if (rc != XMSG_OK)
        {
            return XMSG_EPROTO;
        }
        dg->has_subhdr = true;
        dg->body = info + XMSG_DG_HDR_SIZE;
        dg->body_len = len - XMSG_DG_HDR_SIZE;
    }
    return XMSG_OK;
}

size_t xmsg_dg_write_short(uint8_t *out, uint8_t subtype, uint16_t dst_node, uint16_t src_node,
                           uint16_t flags1, uint16_t flags2)
{
    SintranHeader h;
    sintran_hdr_fill(&h, subtype, dst_node, src_node, flags1, flags2);
    sintran_hdr_write(out, &h);
    return SINTRAN_HDR_SIZE;
}

size_t xmsg_dg_write_data(uint8_t *out, size_t cap, uint8_t subtype, uint16_t dst_node,
                          uint16_t src_node, uint16_t flags1, uint16_t flags2,
                          const XmsgSubhdr *sub, const uint8_t *body, size_t body_len)
{
    SintranHeader h;
    if (out == NULL || sub == NULL || cap < XMSG_DG_HDR_SIZE + body_len)
    {
        return 0;
    }
    sintran_hdr_fill(&h, subtype, dst_node, src_node, flags1, flags2);
    sintran_hdr_write(out, &h);
    xmsg_subhdr_write(out + SINTRAN_HDR_SIZE, sub);
    if (body_len > 0)
    {
        memcpy(out + XMSG_DG_HDR_SIZE, body, body_len);
    }
    return XMSG_DG_HDR_SIZE + body_len;
}

size_t xmsg_dg_write_continuation(uint8_t *out, size_t cap, uint16_t dst_node, uint16_t src_node,
                                  uint16_t flags1, uint16_t resume_offset, const uint8_t *body,
                                  size_t body_len)
{
    SintranHeader h;
    if (out == NULL || cap < SINTRAN_HDR_SIZE + body_len)
    {
        return 0;
    }
    sintran_hdr_fill(&h, (uint8_t)SINTRAN_SUBTYPE_CONTINUATION, dst_node, src_node, flags1,
                     resume_offset);
    sintran_hdr_write(out, &h);
    if (body_len > 0)
    {
        memcpy(out + SINTRAN_HDR_SIZE, body, body_len);
    }
    return SINTRAN_HDR_SIZE + body_len;
}

const char *xmsg_dg_subtype_name(uint8_t subtype)
{
    switch (subtype)
    {
    case SINTRAN_SUBTYPE_ACK:
        return "ACK";
    case SINTRAN_SUBTYPE_ERROR:
        return "ERROR";
    case SINTRAN_SUBTYPE_FIRST_FRAG:
        return "FIRST-FRAGMENT";
    case SINTRAN_SUBTYPE_CONTINUATION:
        return "CONTINUATION";
    case SINTRAN_SUBTYPE_DATA:
        return "DATA";
    case SINTRAN_SUBTYPE_REACH_REPLY:
        return "REACH-REPLY";
    case SINTRAN_SUBTYPE_INNAK:
        return "INNAK";
    case SINTRAN_SUBTYPE_REACH_REQUEST:
        return "REACH-REQUEST";
    default:
        return "?";
    }
}
