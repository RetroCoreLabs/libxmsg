/*
 * xmsg_dg.h - one XMSG datagram as carried in one LAPB information field: parse and build.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Frame shapes (C# XmsgFrame.cs, XmsgFrameBuilder.cs, SecureDatagramReceiver.cs):
 *
 *   short frames   14 bytes, header only: ACK (0x03, Flags1 echoed, Flags2 0x0001), reachability
 *                  request 0x19 and reply 0x13 (Flags1 0xFFFF, Flags2 0x0001), error 0x07
 *                  (Flags1 = the refused number, Flags2 = the negative code).
 *   data           header + sub-header + body (0x0E), body at offset 28, Flags2 = XMCSM.
 *   first fragment header + sub-header + the first 594 body bytes (0x0A), Flags2 = TOTAL length.
 *   continuation   header + the rest of the body (0x0C), NO sub-header, Flags2 = resume offset.
 *
 * Nothing here pads, allocates a Flags1 or touches a link; that is the node's job. This file only
 * knows bytes.
 */
#ifndef XMSG_DG_H
#define XMSG_DG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libxmsg/sintran/sintran_hdr.h"
#include "libxmsg/xmsg/xmsg_subhdr.h"
#include "libxmsg/xmsg_config.h"
#include "libxmsg/xmsg_errors.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Header plus sub-header: where a data body starts. */
#define XMSG_DG_HDR_SIZE (SINTRAN_HDR_SIZE + XMSG_SUBHDR_SIZE)

/** Body bytes a first fragment carries: the largest frame (622) less the 28-byte head. */
#define XMSG_DG_FIRST_FRAG_BODY 594u

/** The longest message: two fragments. No capture shows a third; the longest seen is 1032. */
#define XMSG_DG_MAX_BODY (2u * XMSG_DG_FIRST_FRAG_BODY)

/** Reachability frames carry this in Flags1. It is never a real datagram number. */
#define XMSG_FLAGS1_REACH 0xFFFFu

/** A parsed datagram: views into the frame bytes, nothing copied. */
typedef struct XmsgDatagram
{
    SintranHeader hdr;
    bool has_subhdr;     /**< true for data and first-fragment frames */
    XmsgSubhdr sub;      /**< valid when has_subhdr */
    const uint8_t *body; /**< the body (offset 28 with a sub-header, 14 without) */
    size_t body_len;
    bool repeat; /**< set by the node when this Flags1 was already received from the peer */
} XmsgDatagram;

/**
 * @brief Parse a LAPB information field as a datagram.
 * @param info Bytes.
 * @param len  Count.
 * @param dg   Receives the views.
 * @return XMSG_OK; XMSG_EINVAL when shorter than a header; XMSG_EPROTO on a bad header checksum,
 *         a bad version byte, or a data/first-fragment frame without a valid sub-header.
 */
int xmsg_dg_parse(const uint8_t *info, size_t len, XmsgDatagram *dg);

/**
 * @brief Build a 14-byte header-only frame (ACK, reachability request or reply).
 * @param out      Receives SINTRAN_HDR_SIZE bytes.
 * @param subtype  SINTRAN_SUBTYPE_ACK / REACH_REQUEST / REACH_REPLY.
 * @param dst_node The peer.
 * @param src_node Us.
 * @param flags1   The number to carry: the echoed one for an ACK, XMSG_FLAGS1_REACH otherwise.
 * @param flags2   0x0001 for all three.
 * @return SINTRAN_HDR_SIZE.
 */
size_t xmsg_dg_write_short(uint8_t *out, uint8_t subtype, uint16_t dst_node, uint16_t src_node,
                           uint16_t flags1, uint16_t flags2);

/**
 * @brief Build a data or first-fragment frame: header, sub-header, body.
 * @param out      Output buffer.
 * @param cap      Its size; needs XMSG_DG_HDR_SIZE + body_len.
 * @param subtype  SINTRAN_SUBTYPE_DATA or SINTRAN_SUBTYPE_FIRST_FRAG.
 * @param dst_node The peer.
 * @param src_node Us.
 * @param flags1   The datagram number.
 * @param flags2   XMCSM for data; the total message length for a first fragment.
 * @param sub      The sub-header fields (its xmcsm is written as given).
 * @param body     Body bytes.
 * @param body_len Count.
 * @return Bytes written, or 0 when they do not fit.
 */
size_t xmsg_dg_write_data(uint8_t *out, size_t cap, uint8_t subtype, uint16_t dst_node,
                          uint16_t src_node, uint16_t flags1, uint16_t flags2,
                          const XmsgSubhdr *sub, const uint8_t *body, size_t body_len);

/**
 * @brief Build a continuation frame: header then the rest of the body, no sub-header.
 * @param out           Output buffer.
 * @param cap           Its size; needs SINTRAN_HDR_SIZE + body_len.
 * @param dst_node      The peer.
 * @param src_node      Us.
 * @param flags1        The SAME number as the first fragment.
 * @param resume_offset Where this piece resumes: the first fragment's body length.
 * @param body          The remaining body bytes.
 * @param body_len      Count.
 * @return Bytes written, or 0 when they do not fit.
 */
size_t xmsg_dg_write_continuation(uint8_t *out, size_t cap, uint16_t dst_node, uint16_t src_node,
                                  uint16_t flags1, uint16_t resume_offset, const uint8_t *body,
                                  size_t body_len);

/** @brief Plain-words name of a subtype, for logs. */
const char *xmsg_dg_subtype_name(uint8_t subtype);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_DG_H */
