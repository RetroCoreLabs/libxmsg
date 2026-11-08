/*
 * xmsg_frag.h - joining a long message that arrived as a first fragment and a continuation.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * A message longer than 594 body bytes travels as two frames with the SAME Flags1: a first
 * fragment (0x0A, with the sub-header, Flags2 = total length) and a continuation (0x0C, no
 * sub-header, Flags2 = the offset it resumes at, which must equal the held first part's length or
 * the two do not belong together). The reassembler holds first fragments keyed by (source node,
 * Flags1) and hands out a whole DATA datagram when the continuation arrives. Measured peak
 * outstanding on real traffic is one; the slot count is XMSG_REASSEMBLY_SLOTS and the oldest is
 * evicted when all are busy. C# reference: SintranFragmentReassembler.cs.
 *
 * Splitting for transmission needs no state and lives in the node (two LAPB slots).
 */
#ifndef XMSG_FRAG_H
#define XMSG_FRAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libxmsg/xmsg/xmsg_dg.h"
#include "libxmsg/xmsg_config.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** One held first fragment. */
typedef struct XmsgReasmSlot
{
    bool in_use;
    uint16_t src_node;
    uint16_t flags1;
    uint32_t held_at_ms;
    SintranHeader hdr;
    XmsgSubhdr sub;
    uint16_t len;                   /**< body bytes held */
    uint8_t body[XMSG_DG_MAX_BODY]; /**< the first part, then the joined whole */
} XmsgReasmSlot;

/** The reassembler. */
typedef struct XmsgReassembler
{
    XmsgReasmSlot slots[XMSG_REASSEMBLY_SLOTS];
    uint32_t joined;     /**< messages completed */
    uint32_t orphaned;   /**< continuations with no first fragment held */
    uint32_t mismatched; /**< continuations whose offset did not match the held part */
    uint32_t evicted;    /**< first fragments dropped to make room */
} XmsgReassembler;

/** @brief Empty the reassembler. */
void xmsg_reasm_init(XmsgReassembler *r);

/**
 * @brief Pass a parsed datagram through the reassembler.
 *
 * Whole frames (anything but the two fragment subtypes) pass straight through: `*out = *in`. A
 * first fragment is held and nothing comes out. A continuation that completes a held message
 * produces a DATA datagram whose body lives in the reassembler's slot until the next call that
 * touches that slot.
 *
 * @param r      The reassembler.
 * @param in     The parsed incoming datagram.
 * @param out    Receives the datagram to deliver when the return value is 1.
 * @param now_ms The time, recorded on held fragments.
 * @return 1 when `out` holds a datagram to deliver; 0 when the frame was held; XMSG_EPROTO when a
 *         continuation was dropped (orphan or mismatch, counted).
 */
int xmsg_reasm_accept(XmsgReassembler *r, const XmsgDatagram *in, XmsgDatagram *out,
                      uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_FRAG_H */
