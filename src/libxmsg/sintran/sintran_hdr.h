/*
 * sintran_hdr.h - the 14-byte header in front of every SINTRAN datagram, and its checksum.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Seven big-endian words (registry: DOC/protocols/sintran-wire.json, all MEASURED):
 *
 *   word 0  high byte 0x21 = version 2 / protocol 1 (X5VRS); low byte = HOP COUNT, 0x13 on a
 *           direct link, 0x12 after one relay. Not a marker, although it was long read as one.
 *   word 1  high byte 0x00; low byte the datagram subtype (see SINTRAN_SUBTYPE_*).
 *   word 2  destination node       word 3  source node
 *   word 4  Flags1: the datagram number, one counter per (sender, peer), never reset in use.
 *   word 5  Flags2: scratch the layers reuse - the frame class / XMCSM on a data frame, 0x0001
 *           on an ACK, the negative error code on an error frame, the total length on a first
 *           fragment and the resume offset on a continuation.
 *   word 6  CHECKSUM: the ones-complement sum of words 0..5 with end-around carry, complemented;
 *           a sum of exactly zero is left at zero (the kernel skips the complement). Carved from
 *           the kernel at 137314, verified on 3595 of 3595 captured frames. A wrong checksum
 *           crashes the receiving machine with XMSG ERROR CODE 24, so every header leaves
 *           through sintran_hdr_write(), which computes it, and every header entering is checked.
 */
#ifndef XMSG_SINTRAN_HDR_H
#define XMSG_SINTRAN_HDR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libxmsg/xmsg_errors.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define SINTRAN_HDR_SIZE 14u

#define SINTRAN_VERSION_BYTE 0x21u /**< X5VRS high byte: version 2, protocol 1 */
#define SINTRAN_HOP_DIRECT   0x13u /**< hop count on a direct link */
#define SINTRAN_HOP_RELAYED  0x12u /**< hop count after one relay */
#define SINTRAN_HOP_INNAK    0xFEu /**< seen on INNAK frames */

/* Datagram subtypes (byte 3). */
#define SINTRAN_SUBTYPE_ACK           0x03u /**< 14 bytes: acknowledges the Flags1 it echoes */
#define SINTRAN_SUBTYPE_ERROR         0x07u /**< 14 bytes: Flags2 holds the negative XMSG error */
#define SINTRAN_SUBTYPE_FIRST_FRAG    0x0Au /**< first half of a long message, with sub-header */
#define SINTRAN_SUBTYPE_CONTINUATION  0x0Cu /**< second half, no sub-header */
#define SINTRAN_SUBTYPE_DATA          0x0Eu /**< a whole message */
#define SINTRAN_SUBTYPE_REACH_REPLY   0x13u /**< answer to a reachability request */
#define SINTRAN_SUBTYPE_INNAK         0x17u /**< initialisation NAK; meaning not established */
#define SINTRAN_SUBTYPE_REACH_REQUEST 0x19u /**< "I (re)started": the peer zeroes its count to us */

/* XMSG error codes carried in Flags2 of an error frame, as 16-bit two's complement. */
#define XMSG_XE_XENSE ((int16_t)-34) /**< sequence error: our Flags1 was AHEAD of the expectation */
#define XMSG_XE_XEIMA ((int16_t)-19) /**< invalid magic number: wrong or closed port */

/** A decoded header. */
typedef struct SintranHeader
{
    uint8_t version;   /**< byte 0, normally SINTRAN_VERSION_BYTE */
    uint8_t hop;       /**< byte 1 */
    uint8_t type;      /**< byte 2, normally 0 */
    uint8_t subtype;   /**< byte 3 */
    uint16_t dst_node; /**< word 2 */
    uint16_t src_node; /**< word 3 */
    uint16_t flags1;   /**< word 4 */
    uint16_t flags2;   /**< word 5 */
    uint16_t checksum; /**< word 6 as received, or as computed by sintran_hdr_write() */
} SintranHeader;

/**
 * @brief Compute the checksum word over the first six words of a header.
 * @param hdr14 At least 12 bytes of header; word 6 is not read.
 * @return The checksum as it belongs in word 6.
 */
uint16_t sintran_hdr_checksum(const uint8_t *hdr14);

/**
 * @brief Write a header with its checksum.
 * @param out Receives SINTRAN_HDR_SIZE bytes.
 * @param h   The fields; `checksum` is ignored on input and set on output.
 */
void sintran_hdr_write(uint8_t *out, SintranHeader *h);

/**
 * @brief Fill a header for a frame from us to a peer on a direct link.
 * @param h        Receives the fields; checksum left for sintran_hdr_write().
 * @param subtype  SINTRAN_SUBTYPE_*.
 * @param dst_node The peer.
 * @param src_node Us.
 * @param flags1   The datagram number (or 0xFFFF on reachability frames).
 * @param flags2   The subtype's word 5 value.
 */
void sintran_hdr_fill(SintranHeader *h, uint8_t subtype, uint16_t dst_node, uint16_t src_node,
                      uint16_t flags1, uint16_t flags2);

/**
 * @brief Parse and verify a header.
 * @param in  Bytes.
 * @param len Count; at least SINTRAN_HDR_SIZE.
 * @param h   Receives the fields.
 * @return XMSG_OK; XMSG_EINVAL when too short; XMSG_EPROTO when byte 0 is not 0x21 or the
 *         checksum does not match (the fields are still filled for logging).
 */
int sintran_hdr_parse(const uint8_t *in, size_t len, SintranHeader *h);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_SINTRAN_HDR_H */
