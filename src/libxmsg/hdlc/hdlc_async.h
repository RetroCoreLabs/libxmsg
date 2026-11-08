/*
 * hdlc_async.h - byte-oriented HDLC framing (RFC 1662 style) for TCP bridges and serial gateways.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * A real ND link is a synchronous HDLC line with bit stuffing. Nothing modern has one, so the ways
 * of reaching an ND machine put the frames on a byte stream instead: the nd100x emulator's TCP
 * bridge, and the USB serial side of the HDLCGW (ESP32-S2) and RC422Dev (Pico) gateways. They all
 * use the same framing:
 *
 *   0x7E          flag; one before and one after each frame, runs of flags are idle fill
 *   0x7D 0x5E     a 0x7E inside the frame
 *   0x7D 0x5D     a 0x7D inside the frame
 *   0x7D (x^0x20) any other escaped byte
 *   FCS           the two CRC-16/X-25 bytes are inside the frame, before the closing flag
 *
 * They differ in WHICH bytes the sender escapes. nd100x and the C# reference escape only 0x7E and
 * 0x7D (ITU-T X.25 clause 2.2.2.2; MEASURED across every capture: nothing but 5D and 5E follows a
 * 7D). The USB gateways also escape every byte below 0x20 (the RFC 1662 default ACCM). The decoder
 * does not care - any escaped byte is recovered the same way - so one decoder serves both; the
 * encoder takes the escape set as a 32-bit ACCM mask, bit n set meaning "escape byte n".
 *
 * Decoder rules, from the C# HdlcDecoder and the X.25 clauses it cites:
 *   - bytes before the first flag are discarded (hunting);
 *   - a flag with nothing collected is idle fill and ignored (clause 2.2.4), so flag runs and
 *     their parity do not matter;
 *   - 0x7D followed by 0x7E is an ABORT (clause 2.3.5.3 f): the frame is dropped silently and it
 *     is NOT an FCS error;
 *   - a run shorter than 4 bytes between flags is a runt, not a frame (clause 2.3.5.3 a);
 *   - the FCS is checked when the decoder is told to; a good frame is handed over WITHOUT its FCS
 *     bytes, which is what LAPB wants;
 *   - a run longer than the buffer is dropped at its closing flag and counted.
 *
 * Neither side allocates: the decoder works in a buffer the caller owns, and the encoder is
 * resumable so it can fill whatever space a non-blocking transport has right now.
 */
#ifndef XMSG_HDLC_ASYNC_H
#define XMSG_HDLC_ASYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libxmsg/hdlc/fcs16.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define HDLC_FLAG        0x7Eu /**< frame delimiter */
#define HDLC_ESCAPE      0x7Du /**< the next byte is XOR 0x20 */
#define HDLC_ESCAPE_MASK 0x20u /**< applied to an escaped byte */

/** Escape only 0x7E and 0x7D: nd100x, the C# reference, the real ND machines. */
#define HDLC_ACCM_NONE 0x00000000u
/** Also escape every byte below 0x20: the HDLCGW and RC422Dev USB gateways, RFC 1662 default. */
#define HDLC_ACCM_ALL_CONTROL 0xFFFFFFFFu

/** The smallest run between flags that can be a frame: address, control, two FCS bytes. */
#define HDLC_MIN_FRAME_WITH_FCS 4u

/** What one call to hdlc_decoder_feed() ended with. Framing errors are values, never faults. */
typedef enum HdlcDecodeStatus
{
    HDLC_DECODE_NEED_MORE = 0, /**< the input was consumed, no frame finished yet */
    HDLC_DECODE_FRAME = 1,     /**< a good frame is ready: hdlc_decoder_frame() */
    HDLC_DECODE_FCS_ERROR = 2, /**< a run ended with a bad FCS; dropped and counted */
    HDLC_DECODE_RUNT = 3,      /**< fewer than HDLC_MIN_FRAME_WITH_FCS bytes between flags */
    HDLC_DECODE_TOO_LONG = 4,  /**< a run outgrew the buffer; dropped at its closing flag */
    HDLC_DECODE_ABORTED = 5    /**< 0x7D 0x7E: the sender abandoned the frame */
} HdlcDecodeStatus;

/** Counters kept across frames; read them for diagnostics, reset with hdlc_decoder_init(). */
typedef struct HdlcDecoderStats
{
    uint32_t frames;     /**< good frames delivered */
    uint32_t fcs_errors; /**< runs with a bad FCS */
    uint32_t runts;      /**< runs too short to be a frame */
    uint32_t too_long;   /**< runs that outgrew the buffer */
    uint32_t aborts;     /**< 0x7D 0x7E sequences */
    uint32_t idle_flags; /**< flags that closed nothing (inter-frame fill) */
} HdlcDecoderStats;

/** Decoder state. Treat every field as private; use the functions. */
typedef struct HdlcDecoder
{
    uint8_t *buf;     /**< caller's buffer, holds the frame being collected */
    size_t cap;       /**< its size */
    size_t len;       /**< bytes collected so far, or of the delivered frame */
    size_t frame_len; /**< length handed out by hdlc_decoder_frame(); 0 when none */
    bool check_fcs;   /**< verify and strip the FCS (true) or hand the raw run out (false) */
    bool seen_flag;   /**< an opening flag has been seen; collecting */
    bool escaped;     /**< the previous byte was 0x7D */
    bool overflowed;  /**< the current run no longer fits; drop it at the next flag */
    HdlcDecoderStats stats;
} HdlcDecoder;

/**
 * @brief Set up a decoder over a caller-owned buffer.
 * @param d         Decoder to set up.
 * @param buf       Storage for one frame (with FCS). For LAPB use XMSG_LAPB_N1 + 4 bytes.
 * @param cap       Its size in bytes; must be at least HDLC_MIN_FRAME_WITH_FCS.
 * @param check_fcs true to verify the FCS and deliver frames without it; false to deliver the raw
 *                  bytes between flags (a frame of any length >= 1), for tools that inspect them.
 * @return 0, or -1 for a NULL or too small buffer.
 */
int hdlc_decoder_init(HdlcDecoder *d, uint8_t *buf, size_t cap, bool check_fcs);

/**
 * @brief Drop any partial frame and hunt for a flag again, as after a line drop. Counters stay.
 * @param d Decoder.
 */
void hdlc_decoder_reset(HdlcDecoder *d);

/**
 * @brief Feed received bytes. Stops after the first frame boundary so the caller can act on it.
 *
 * Drain a read that may hold several frames like this:
 * @code
 *   size_t off = 0;
 *   while (off < n) {
 *       size_t used;
 *       HdlcDecodeStatus st = hdlc_decoder_feed(&d, buf + off, n - off, &used);
 *       off += used;
 *       if (st == HDLC_DECODE_FRAME) { handle(hdlc_decoder_frame(&d, &len), len); }
 *   }
 * @endcode
 *
 * @param d        Decoder.
 * @param in       Received bytes.
 * @param len      How many.
 * @param consumed Receives how many of them were used; equals len on HDLC_DECODE_NEED_MORE, and
 *                 otherwise stops just past the flag that ended the run.
 * @return What happened; see HdlcDecodeStatus.
 */
HdlcDecodeStatus hdlc_decoder_feed(HdlcDecoder *d, const uint8_t *in, size_t len, size_t *consumed);

/**
 * @brief The frame delivered by the last HDLC_DECODE_FRAME, valid until the next feed.
 * @param d   Decoder.
 * @param len Receives its length (FCS already removed when check_fcs is on).
 * @return Pointer into the caller's buffer, or NULL when no frame is pending.
 */
const uint8_t *hdlc_decoder_frame(const HdlcDecoder *d, size_t *len);

/**
 * @brief The longest byte stream a frame of this length can encode to.
 *
 * Every byte may need escaping, the FCS too, plus two flags. Size the output buffer of
 * hdlc_encode() with this when you want a one-shot encode to always succeed.
 *
 * @param frame_len Length of the frame to be sent (without FCS when the encoder adds it).
 * @return Bytes.
 */
static inline size_t hdlc_encoded_max(size_t frame_len)
{
    return 2u * (frame_len + FCS16_SIZE) + 2u;
}

/** Resumable encoder state. Treat every field as private; use the functions. */
typedef struct HdlcEncoder
{
    uint32_t accm;        /**< which bytes below 0x20 to escape, bit n for byte n */
    bool add_fcs;         /**< compute and append the FCS (true) or send the frame as given */
    const uint8_t *frame; /**< the frame being sent; must stay valid until done */
    size_t len;           /**< its length */
    size_t pos;           /**< next frame byte to emit */
    uint16_t fcs;         /**< running register, then the two FCS bytes once computed */
    uint8_t fcs_bytes[2]; /**< the FCS as sent, low byte first */
    uint8_t stage;        /**< 0 idle, 1 open flag, 2 body, 3 fcs, 4 close flag */
    bool half_escape;     /**< 0x7D went out, the escaped byte itself is still owed */
    uint8_t owed;         /**< that escaped byte (already XOR 0x20) */
} HdlcEncoder;

/**
 * @brief Set up an encoder.
 * @param e       Encoder.
 * @param accm    HDLC_ACCM_NONE for nd100x and real ND machines, HDLC_ACCM_ALL_CONTROL for the USB
 *                gateways, or any other mask.
 * @param add_fcs true when the caller gives frames without FCS and the encoder must append it;
 *                false when the frame already carries one (a gateway forwarding as-is).
 */
void hdlc_encoder_init(HdlcEncoder *e, uint32_t accm, bool add_fcs);

/**
 * @brief Start sending a frame. The frame memory must stay valid until hdlc_encoder_done().
 * @param e     Encoder; must be idle (hdlc_encoder_done() true).
 * @param frame The frame bytes: address, control, information.
 * @param len   Their count; at least 1.
 * @return 0, or -1 when busy or the arguments are bad.
 */
int hdlc_encoder_begin(HdlcEncoder *e, const uint8_t *frame, size_t len);

/**
 * @brief Produce as many output bytes as fit. Call again when more room exists.
 * @param e   Encoder.
 * @param out Output buffer.
 * @param cap Its size.
 * @return Bytes written; 0 when the frame is finished (or nothing was begun).
 */
size_t hdlc_encoder_produce(HdlcEncoder *e, uint8_t *out, size_t cap);

/**
 * @brief Whether the encoder has nothing left to emit.
 * @param e Encoder.
 * @return true when idle.
 */
bool hdlc_encoder_done(const HdlcEncoder *e);

/**
 * @brief One-shot encode of a whole frame into a buffer.
 * @param frame   Frame bytes (without FCS when add_fcs is true).
 * @param len     Their count.
 * @param accm    Escape set, as for hdlc_encoder_init().
 * @param add_fcs Append the FCS or not.
 * @param out     Output buffer; hdlc_encoded_max(len) bytes always suffice.
 * @param cap     Its size.
 * @return Bytes written, or 0 when the output did not fit or len is 0.
 */
size_t hdlc_encode(const uint8_t *frame, size_t len, uint32_t accm, bool add_fcs, uint8_t *out,
                   size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_HDLC_ASYNC_H */
