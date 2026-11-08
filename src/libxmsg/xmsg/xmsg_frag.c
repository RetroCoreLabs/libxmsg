/*
 * xmsg_frag.c - the fragment reassembler.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/xmsg/xmsg_frag.h"

#include <string.h>

#include "libxmsg/log/xmsg_log.h"

void xmsg_reasm_init(XmsgReassembler *r)
{
    memset(r, 0, sizeof *r);
}

static XmsgReasmSlot *find_slot(XmsgReassembler *r, uint16_t src_node, uint16_t flags1)
{
    for (int i = 0; i < XMSG_REASSEMBLY_SLOTS; i++)
    {
        XmsgReasmSlot *s = &r->slots[i];
        if (s->in_use && s->src_node == src_node && s->flags1 == flags1)
        {
            return s;
        }
    }
    return NULL;
}

static XmsgReasmSlot *take_slot(XmsgReassembler *r)
{
    XmsgReasmSlot *oldest = &r->slots[0];
    for (int i = 0; i < XMSG_REASSEMBLY_SLOTS; i++)
    {
        XmsgReasmSlot *s = &r->slots[i];
        if (!s->in_use)
        {
            return s;
        }
        if ((int32_t)(s->held_at_ms - oldest->held_at_ms) < 0)
        {
            oldest = s;
        }
    }
    r->evicted++;
    XMSG_LOG(XMSG_LOG_FRAG, XMSG_LOG_WARN,
             "holding %d incomplete message(s); dropping the oldest (node %u Flags1 0x%04X)",
             XMSG_REASSEMBLY_SLOTS, oldest->src_node, oldest->flags1);
    oldest->in_use = false;
    return oldest;
}

int xmsg_reasm_accept(XmsgReassembler *r, const XmsgDatagram *in, XmsgDatagram *out,
                      uint32_t now_ms)
{
    if (in->hdr.subtype == SINTRAN_SUBTYPE_FIRST_FRAG)
    {
        XmsgReasmSlot *s = find_slot(r, in->hdr.src_node, in->hdr.flags1);
        if (s == NULL)
        {
            s = take_slot(r);
        }
        if (in->body_len > XMSG_DG_MAX_BODY)
        {
            return XMSG_EPROTO;
        }
        s->in_use = true;
        s->src_node = in->hdr.src_node;
        s->flags1 = in->hdr.flags1;
        s->held_at_ms = now_ms;
        s->hdr = in->hdr;
        s->sub = in->sub;
        s->len = (uint16_t)in->body_len;
        memcpy(s->body, in->body, in->body_len);
        return 0;
    }

    if (in->hdr.subtype == SINTRAN_SUBTYPE_CONTINUATION)
    {
        XmsgReasmSlot *s = find_slot(r, in->hdr.src_node, in->hdr.flags1);
        if (s == NULL)
        {
            r->orphaned++;
            XMSG_LOG(
                XMSG_LOG_FRAG, XMSG_LOG_WARN,
                "continuation from node %u at Flags1 0x%04X with no first fragment held; dropped",
                in->hdr.src_node, in->hdr.flags1);
            return XMSG_EPROTO;
        }
        if (in->hdr.flags2 != s->len || (size_t)s->len + in->body_len > XMSG_DG_MAX_BODY)
        {
            r->mismatched++;
            XMSG_LOG(
                XMSG_LOG_FRAG, XMSG_LOG_WARN,
                "continuation from node %u resumes at %u but the held part has %u byte(s); dropped",
                in->hdr.src_node, in->hdr.flags2, s->len);
            s->in_use = false;
            return XMSG_EPROTO;
        }
        memcpy(s->body + s->len, in->body, in->body_len);
        s->len = (uint16_t)(s->len + in->body_len);
        s->in_use = false;
        r->joined++;

        memset(out, 0, sizeof *out);
        out->hdr = s->hdr;
        out->hdr.subtype = (uint8_t)SINTRAN_SUBTYPE_DATA;
        out->has_subhdr = true;
        out->sub = s->sub;
        out->body = s->body;
        out->body_len = s->len;
        return 1;
    }

    *out = *in;
    return 1;
}
