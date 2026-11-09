/*
 * lapb_frame.h - reading and writing the address and control bytes of an ND LAPB frame.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * The ND dialect of LAPB (ND spec section 2; C# reference RetroIO.Lapb LapbControl.cs and
 * LapbFrame.cs), as measured on real ND-100 links:
 *
 *   address  0x01 on link-management frames: SABM, UA, DISC, DM, FRMR
 *            0x09 on data-transfer frames: I, RR, RNR, REJ
 *            0x89 on an I-frame whose information field has an ODD byte count. The ND-100 DMAs
 *                 frames in 16-bit words and silently discards an I-frame whose address parity
 *                 disagrees with its length, so this bit is set per frame, never left at 0x09.
 *   control  I-frame  : 0 N(S) N(S) N(S) P N(R) N(R) N(R)   bit 0 clear
 *            S-frame  : 1 0 nibble      P/F N(R)            bits 0..1 = 01; nibble RR 1, RNR 5, REJ 9
 *            U-frame  : 1 1 pattern     P/F pattern         bits 0..1 = 11; pattern with P/F cleared
 *                       SABM 0x2F, DISC 0x43, UA 0x63, DM 0x0F, FRMR 0x87
 *   info     U and S frames carry the SENDER'S NODE NUMBER as two big-endian bytes - this is how a
 *            neighbour is identified on an ND link (spec 2.3.1 / 3.3). FRMR carries its 3-byte
 *            diagnostic instead. I-frames carry the datagram.
 *
 * Everything here is `static inline`: no state, no allocation, usable from tests and tools.
 */
#ifndef XMSG_LAPB_FRAME_H
#define XMSG_LAPB_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define LAPB_ADDR_LINK    0x01u /**< SABM, UA, DISC, DM, FRMR */
#define LAPB_ADDR_DATA    0x09u /**< I, RR, RNR, REJ with an even information length */
#define LAPB_ADDR_ODD_BIT 0x80u /**< OR-ed onto LAPB_ADDR_DATA when the I-field length is odd */
#define LAPB_PF_BIT       0x10u /**< poll (command) / final (response) bit in the control byte */

#define LAPB_U_SABM 0x2Fu /**< set asynchronous balanced mode: start the link */
#define LAPB_U_DISC 0x43u /**< disconnect */
#define LAPB_U_UA   0x63u /**< unnumbered acknowledgement: yes to SABM or DISC */
#define LAPB_U_DM   0x0Fu /**< disconnected mode: I am down */
#define LAPB_U_FRMR 0x87u /**< frame reject: your frame made no sense, here is why */

#define LAPB_S_RR  0x01u /**< receive ready, acknowledging through N(R) */
#define LAPB_S_RNR 0x05u /**< receive not ready: stop sending */
#define LAPB_S_REJ 0x09u /**< reject: go back to N(R) */

/* FRMR diagnostic reason bits (spec 2.3.3). Reason X (0x02) is neither sent nor recognised. */
#define LAPB_FRMR_W 0x01u /**< control field invalid or not implemented */
#define LAPB_FRMR_Y 0x04u /**< information field too long */
#define LAPB_FRMR_Z 0x08u /**< N(R) invalid */

/** The kind of a frame, decided by the two low bits of the control byte. */
typedef enum LapbFrameKind
{
    LAPB_FRAME_INFORMATION = 0,
    LAPB_FRAME_SUPERVISORY = 1,
    LAPB_FRAME_UNNUMBERED = 2
} LapbFrameKind;

/** A decoded frame: views into the caller's bytes, nothing copied. */
typedef struct LapbFrameView
{
    uint8_t address;     /**< the address byte as received */
    uint8_t control;     /**< the control byte as received */
    LapbFrameKind kind;  /**< I, S or U */
    uint8_t ns;          /**< N(S) for an I-frame, else 0 */
    uint8_t nr;          /**< N(R) for I and S frames, else 0 */
    bool pf;             /**< the P/F bit */
    uint8_t s_type;      /**< LAPB_S_RR / RNR / REJ, or the raw nibble when unknown */
    uint8_t u_type;      /**< LAPB_U_* with the P/F bit cleared */
    const uint8_t *info; /**< the information field (node number, diagnostic, datagram) */
    size_t info_len;     /**< its length */
} LapbFrameView;

/**
 * @brief Decode a frame (address .. info, FCS already removed and verified).
 * @param frame Bytes.
 * @param len   Count; must be at least 2.
 * @param view  Receives the decoded view.
 * @return true when the frame is at least address + control. The address and the control pattern
 *         are NOT validated here; the link layer decides what an unknown one means.
 */
static inline bool lapb_frame_parse(const uint8_t *frame, size_t len, LapbFrameView *view)
{
    if (frame == NULL || view == NULL || len < 2)
    {
        return false;
    }
    view->address = frame[0];
    view->control = frame[1];
    view->pf = (frame[1] & LAPB_PF_BIT) != 0;
    view->ns = 0;
    view->nr = 0;
    view->s_type = 0;
    view->u_type = 0;
    if ((frame[1] & 0x01u) == 0)
    {
        view->kind = LAPB_FRAME_INFORMATION;
        view->ns = (uint8_t)((frame[1] >> 1) & 0x07u);
        view->nr = (uint8_t)((frame[1] >> 5) & 0x07u);
    }
    else if ((frame[1] & 0x03u) == 0x01u)
    {
        view->kind = LAPB_FRAME_SUPERVISORY;
        view->nr = (uint8_t)((frame[1] >> 5) & 0x07u);
        view->s_type = (uint8_t)(frame[1] & 0x0Fu);
    }
    else
    {
        view->kind = LAPB_FRAME_UNNUMBERED;
        view->u_type = (uint8_t)(frame[1] & (uint8_t)~LAPB_PF_BIT);
    }
    view->info = frame + 2;
    view->info_len = len - 2;
    return true;
}

/**
 * @brief Whether an address byte has the shape a SINTRAN machine builds: bit 0 set, bits 4..6
 *        clear, bit 7 the odd-length marker, bits 1..3 the sender's receive window.
 *
 * SINTRAN's XMSG kernel routine XHSTR (143630 octal, carved segment 076-S3XMK of L-VSX-500)
 * builds the address of every I, RR and REJ frame as ((max(XLRMR - 1, 0) & 7) << 1) | 1, where
 * XLRMR is the number of empty frame buffers queued to its HDLC receiver, and ORs in 0x80 when
 * an I-frame's byte count is odd; SABM and UA get 0x01. So 0x09 = window 4 (all 5 buffers free,
 * the normal case), 0x07 = window 3, 0x05 = 2, 0x03 = 1, 0x01 = 0. Read from the code and
 * checked against every capture, 2026-11-09 (NDInsight
 * SINTRAN/XMSG/DOC/HDLC-ADDRESS-BYTE-ANALYSIS-2026-11-09.md). A frame with another address is
 * still taken, as the C# does; see lapb_rx_frame().
 */
static inline bool lapb_frame_address_fits(uint8_t address)
{
    return (address & 0x71u) == 0x01u;
}

/** @brief The receive window a peer announces in the address of an I or S frame: bits 1..3. */
static inline uint8_t lapb_frame_peer_window(uint8_t address)
{
    return (uint8_t)((address >> 1) & 0x07u);
}

/** @brief The address byte for an I-frame carrying `info_len` bytes: 0x09 even, 0x89 odd. */
static inline uint8_t lapb_frame_data_address(size_t info_len)
{
    return (uint8_t)(LAPB_ADDR_DATA | (((info_len & 1u) != 0) ? LAPB_ADDR_ODD_BIT : 0u));
}

/** @brief The control byte of an I-frame: N(R) in bits 5..7, P in bit 4, N(S) in bits 1..3. */
static inline uint8_t lapb_frame_i_control(uint8_t ns, uint8_t nr, bool poll)
{
    return (uint8_t)(((nr & 0x07u) << 5) | (poll ? LAPB_PF_BIT : 0u) | ((ns & 0x07u) << 1));
}

/** @brief The control byte of an S-frame: N(R) in bits 5..7, P/F in bit 4, the type nibble. */
static inline uint8_t lapb_frame_s_control(uint8_t s_type, uint8_t nr, bool pf)
{
    return (uint8_t)(((nr & 0x07u) << 5) | (pf ? LAPB_PF_BIT : 0u) | (s_type & 0x0Fu));
}

/** @brief The control byte of a U-frame: the pattern with the P/F bit set or not. */
static inline uint8_t lapb_frame_u_control(uint8_t u_type, bool pf)
{
    return (uint8_t)(u_type | (pf ? LAPB_PF_BIT : 0u));
}

#ifdef __cplusplus
}
#endif

#endif /* XMSG_LAPB_FRAME_H */
