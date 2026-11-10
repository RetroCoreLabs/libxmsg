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

typedef struct
{
    int16_t code;
    const char *text;
} SintranErrorName;

// clang-format off
static const SintranErrorName s_error_names[] = {
    {-1, "XENOT No more XT-blocks free"},
    {-2, "XEIRM Non-local remote port illegal here"},
    {-3, "XENIR Network layer initialization reject (purely internal)"},
    {-4, "XETMM Task is not allowed any more memory"},
    {-5, "XENIM Facility not yet implemented"},
    {-6, "XEIBP Illegal message buffer pointer"},
    {-7, "XEBNY Message buffer not yours"},
    {-8, "XEIRT Illegal function for RT-programs (only drivers)"},
    {-9, "XENOP No more ports available"},
    {-10, "XEIDR Function not available to drivers"},
    {-11, "XENDM No default message"},
    {-12, "XEMCH Message is already chained"},
    {-13, "XEBFC Message is in a queue."},
    {-14, "XEAIN XMSG Kernel already initialised"},
    {-15, "XEBNC Return of a bounce message"},
    {-16, "XEWNA Write Not Allowed (Indirect buffer)"},
    {-17, "XENVI No Valid Indirect buffer defined"},
    {-18, "XEILF Illegal function code"},
    {-19, "XEIMA Invalid magic number"},
    {-20, "XEMFL Message space full or buffer not available"},
    {-21, "XEILM Illegal message size or not enough space left"},
    {-22, "XEIPN Illegal port number"},
    {-23, "XEPRV Privileged function called without privilege."},
    {-24, "XEPVR Privilege request refused"},
    {-25, "XERNA Remote system not available"},
    {-26, "XEROV Remote task space overflow"},
    {-27, "XEXBF Message already has XMSG buffer (XFDUB)"},
    {-28, "XEDRI Driver called XMSG before return from previous"},
    {-29, "XENDP No port open (so 'default port' param invalid)"},
    {-30, "XEITL Illegal transfer length for read/write"},
    {-31, "XEIDP Illegal displacement in read/write"},
    {-32, "XEILR Illegal use of reentrant segment in XFDIB"},
    {-33, "XENOS Indirect Buffer not on valid segment"},
    {-34, "XENSE Network sequencing error (purely internal significance)"},
    {-35, "XERND Remote system not defined"},
    {-36, "XEPCL Remote port closed while message queued"},
    {-37, "XENRU XMSG not running  do not move"},
    {-38, "XENTO Timeout detected by network layer"},
    {-39, "XENUS No user segment info defined (XFUSG option)"},
    {-40, "XEREJ Network remote reject (request retransmit)"},
    {-41, "XEIXT Driver called XMSG with illegal XT-block"},
    {-42, "XETMU Too many multi-call functions"},
    {-43, "XENCE Network checksum error (purely internal significance)"},
    {-44, "XEICM Function not available to tasks other than COSROUT"},
    {-63, "XECRA XMSG user error codes (XE*), returned negative in the T-register. /// </summary> public enum XmsgError : int { /// <summary> /// XMSG crash (Info in Basefield)"},
};
// clang-format on

const char *sintran_error_name(int16_t code)
{
    for (size_t i = 0; i < sizeof s_error_names / sizeof s_error_names[0]; i++)
    {
        if (s_error_names[i].code == code)
        {
            return s_error_names[i].text;
        }
    }
    return "unknown XMSG error";
}
