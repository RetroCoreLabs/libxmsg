/*
 * xmsg_subhdr.h - the 14-byte XMSG sub-header that follows the SINTRAN header on a data frame.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Absolute offsets 14..27 of a data frame (C# reference XmsgSubHeader.cs; registry
 * sintran-wire.json bitfields):
 *
 *   14  0x21 0x00   marker (XMTHD)
 *   16  frame flags: copy a CAPTURED combination, never compose bits - 0x86 setup, 0x82 bare
 *       control, 0x96 data A, 0x92 data B (the rule for bit 0x04 is not known)
 *   17  role / send-option byte (the ROLE byte on the wire)
 *   18  destination system   20  destination port word
 *   22  source system        24  source port word
 *   26  XMCSM: on most data frames the body length; on a letter the class word (0x0400)
 *
 * The body starts at offset 28. A port word is `(index << 7) | 7-bit random`; a peer's port word
 * is learned from its frames, never computed.
 */
#ifndef XMSG_SUBHDR_H
#define XMSG_SUBHDR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libxmsg/xmsg_errors.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define XMSG_SUBHDR_SIZE    14u
#define XMSG_SUBHDR_MARKER0 0x21u
#define XMSG_SUBHDR_MARKER1 0x00u

/* Frame-flag combinations as captured (registry xmsg_frame_flags.combos). */
#define XMSG_FF_SETUP        0x86u /**< connect letters, session setup, accepts */
#define XMSG_FF_CONTROL_BARE 0x82u /**< ESCA, DCON, the 0xFD notify */
#define XMSG_FF_DATA_A       0x96u /**< terminal data, one of two observed forms */
#define XMSG_FF_DATA_B       0x92u /**< terminal data, the other form */

/** The sub-header fields. */
typedef struct XmsgSubhdr
{
    uint8_t frame_flags;
    uint8_t role;
    uint16_t dst_system;
    uint16_t dst_port;
    uint16_t src_system;
    uint16_t src_port;
    uint16_t xmcsm;
} XmsgSubhdr;

/**
 * @brief Write a sub-header.
 * @param out Receives XMSG_SUBHDR_SIZE bytes.
 * @param s   The fields.
 */
void xmsg_subhdr_write(uint8_t *out, const XmsgSubhdr *s);

/**
 * @brief Parse a sub-header.
 * @param in  Bytes at the sub-header's start (absolute offset 14 of the frame).
 * @param len Bytes available; at least XMSG_SUBHDR_SIZE.
 * @param s   Receives the fields.
 * @return XMSG_OK; XMSG_EINVAL when too short; XMSG_EPROTO when the marker is not 21 00.
 */
int xmsg_subhdr_parse(const uint8_t *in, size_t len, XmsgSubhdr *s);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_SUBHDR_H */
