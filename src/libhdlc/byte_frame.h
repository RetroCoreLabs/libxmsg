/**************************************************************************
** BYTE_FRAME - HDLC frames on a byte stream (RFC 1662 style)            **
**                                                                       **
** A real ND link is a synchronous HDLC line with bit stuffing (hdlc.h). **
** Everything else that reaches an ND machine puts the frames on a byte  **
** stream instead: the USB serial side of HDLCGW and RC422Dev, a TCP     **
** peer of HDLCGW, and the nd100x emulator's TCP bridge. They all use    **
** the same framing:                                                     **
**                                                                       **
**   0x7E            flag; one before and one after each frame, runs of  **
**                   flags are idle fill                                 **
**   0x7D 0x5E       a 0x7E inside the frame                             **
**   0x7D 0x5D       a 0x7D inside the frame                             **
**   0x7D (x ^ 0x20) any other escaped byte                              **
**                                                                       **
** They differ in WHICH bytes the sender escapes. nd100x, the C#         **
** reference and the real ND machines escape only 0x7E and 0x7D (ITU-T   **
** X.25 clause 2.2.2.2; measured across every capture: nothing but 5D    **
** and 5E follows a 7D). The USB gateways also escape every byte below   **
** 0x20 (the RFC 1662 default ACCM). The decoder does not care - any     **
** escaped byte comes back the same way - so one decoder serves both;    **
** the encoder takes the escape set as a 32-bit ACCM mask, bit n set     **
** meaning "escape byte n".                                              **
**                                                                       **
** Decoder rules, from the C# HdlcDecoder and the X.25 clauses it cites: **
**  - bytes before the first flag are discarded (hunting);               **
**  - a flag with nothing collected is idle fill and ignored (clause     **
**    2.2.4), so flag runs and their parity do not matter;               **
**  - 0x7D followed by 0x7E is an ABORT (clause 2.3.5.3 f): the frame is **
**    dropped and it is NOT an FCS error;                                **
**  - the byte after any other 0x7D is XORed with 0x20, whatever it is   **
**    (RFC 1662 4.2);                                                    **
**  - with the FCS check on, a run shorter than 4 bytes is a runt, not a **
**    frame (clause 2.3.5.3 a), and a good frame is handed over WITHOUT  **
**    its FCS bytes, which is what LAPB wants;                           **
**  - a run longer than the buffer is dropped at its closing flag and    **
**    counted.                                                           **
**                                                                       **
** Two ways in:                                                          **
**  - byte_frame_rx / byte_frame_tx: the full engine. Caller-owned       **
**    buffer, FCS check on or off, any escape set, a resumable encoder   **
**    for a non-blocking transport. libxmsg uses this.                   **
**  - byte_frame_decoder / byte_frame_encode(): the short form HDLCGW    **
**    and RC422Dev use between the gateway and the PC: a fixed           **
**    BYTE_FRAME_MAX buffer inside the decoder, FCS passed through       **
**    unchecked (the host checks end to end), every byte below 0x20      **
**    escaped. Built on the same engine.                                 **
**                                                                       **
** The engine came from libxmsg (hdlc_async, mirroring RetroIO.Hdlc      **
** HdlcDecoder.cs / HdlcEncoder.cs) on 2026-11-09, when Ronny ruled that **
** all HDLC code lives in libhdlc only. Nothing allocates and nothing    **
** here depends on Arduino.                                              **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#ifndef BYTE_FRAME_H
#define BYTE_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libhdlc/hdlc.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define BYTE_FRAME_FLAG        0x7Eu /**< frame delimiter */
#define BYTE_FRAME_ESCAPE      0x7Du /**< next byte is XOR 0x20 */
#define BYTE_FRAME_ESCAPE_MASK 0x20u /**< applied to an escaped byte */

/** Escape only 0x7E and 0x7D: nd100x, the C# reference, the real ND machines. */
#define BYTE_FRAME_ACCM_NONE 0x00000000u
/** Also escape every byte below 0x20: the HDLCGW and RC422Dev USB side, RFC 1662 default. */
#define BYTE_FRAME_ACCM_ALL_CONTROL 0xFFFFFFFFu

/** The smallest run between flags that can be a frame: address, control, two FCS bytes. */
#define BYTE_FRAME_MIN_WITH_FCS 4u

/* ================================================================ engine */

/** What one call to byte_frame_rx_feed() ended with. Framing errors are values, never faults. */
typedef enum
{
    BYTE_FRAME_RX_NEED_MORE = 0, /**< the input was consumed, no frame finished yet */
    BYTE_FRAME_RX_FRAME = 1,     /**< a frame is ready: byte_frame_rx_frame() */
    BYTE_FRAME_RX_FCS_ERROR = 2, /**< a run ended with a bad FCS; dropped and counted */
    BYTE_FRAME_RX_RUNT = 3,      /**< fewer than BYTE_FRAME_MIN_WITH_FCS bytes between flags */
    BYTE_FRAME_RX_TOO_LONG = 4,  /**< a run outgrew the buffer; dropped at its closing flag */
    BYTE_FRAME_RX_ABORTED = 5    /**< 0x7D 0x7E: the sender abandoned the frame */
} byte_frame_rx_status;

/** Counters kept across frames; cleared only by byte_frame_rx_init(). */
typedef struct
{
    uint32_t frames;     /**< frames delivered */
    uint32_t fcs_errors; /**< runs with a bad FCS */
    uint32_t runts;      /**< runs too short to be a frame */
    uint32_t too_long;   /**< runs that outgrew the buffer */
    uint32_t aborts;     /**< 0x7D 0x7E sequences */
    uint32_t idle_flags; /**< flags that closed nothing (inter-frame fill) */
} byte_frame_rx_stats;

/** Decoder state. Treat every field as private; use the functions. */
typedef struct
{
    uint8_t *buf;     /**< caller's buffer, holds the frame being collected */
    size_t cap;       /**< its size */
    size_t len;       /**< bytes collected so far */
    size_t frame_len; /**< length handed out by byte_frame_rx_frame(); 0 when none */
    bool check_fcs;   /**< verify and strip the FCS (true) or hand the raw run out (false) */
    bool seen_flag;   /**< an opening flag has been seen; collecting */
    bool escaped;     /**< the previous byte was 0x7D */
    bool overflowed;  /**< the current run no longer fits; drop it at the next flag */
    byte_frame_rx_stats stats;
} byte_frame_rx;

/**
 * @brief Set up a decoder over a caller-owned buffer and clear its counters.
 * @param[out] d          Decoder.
 * @param[in]  buf        Storage for one frame (with FCS). For LAPB use N1 + 4 bytes.
 * @param[in]  cap        Its size in bytes; at least BYTE_FRAME_MIN_WITH_FCS.
 * @param[in]  check_fcs  true to verify the FCS and deliver frames without it; false to deliver
 *                        the raw bytes between flags (any run of 1 byte or more), FCS included.
 * @return 0, or -1 for a NULL decoder or buffer, or a too small buffer.
 */
int byte_frame_rx_init(byte_frame_rx *d, uint8_t *buf, size_t cap, bool check_fcs);

/**
 * @brief Drop any partial frame and hunt for a flag again, as after a line drop. Counters stay.
 * @param[in,out] d  Decoder.
 */
void byte_frame_rx_reset(byte_frame_rx *d);

/**
 * @brief Feed received bytes. Stops after the first frame boundary so the caller can act on it.
 *
 * Drain a read that may hold several frames like this:
 * @code
 *   size_t off = 0;
 *   while (off < n) {
 *       size_t used;
 *       byte_frame_rx_status st = byte_frame_rx_feed(&d, buf + off, n - off, &used);
 *       off += used;
 *       if (st == BYTE_FRAME_RX_FRAME) { handle(byte_frame_rx_frame(&d, &len), len); }
 *   }
 * @endcode
 *
 * @param[in,out] d         Decoder.
 * @param[in]     in        Received bytes.
 * @param[in]     len       How many.
 * @param[out]    consumed  How many of them were used; equals len on BYTE_FRAME_RX_NEED_MORE,
 *                          and otherwise stops just past the flag that ended the run.
 * @return What happened; see byte_frame_rx_status.
 */
byte_frame_rx_status byte_frame_rx_feed(byte_frame_rx *d, const uint8_t *in, size_t len,
                                        size_t *consumed);

/**
 * @brief The frame delivered by the last BYTE_FRAME_RX_FRAME, valid until the next feed.
 * @param[in]  d    Decoder.
 * @param[out] len  Its length (FCS already removed when check_fcs is on).
 * @return Pointer into the caller's buffer, or NULL when no frame is pending.
 */
const uint8_t *byte_frame_rx_frame(const byte_frame_rx *d, size_t *len);

/**
 * @brief The longest byte stream a frame of this length can encode to: every byte escaped, the
 *        FCS too, plus two flags. A one-shot encode into this many bytes always succeeds.
 * @param[in] frame_len  Length of the frame (without FCS when the encoder adds it).
 * @return Bytes.
 */
static inline size_t byte_frame_encoded_max(size_t frame_len)
{
    return 2u * (frame_len + HDLC_FCS_SIZE) + 2u;
}

/** Resumable encoder state. Treat every field as private; use the functions. */
typedef struct
{
    uint32_t accm;        /**< which bytes below 0x20 to escape, bit n for byte n */
    bool add_fcs;         /**< compute and append the FCS (true) or send the frame as given */
    const uint8_t *frame; /**< the frame being sent; must stay valid until done */
    size_t len;           /**< its length */
    size_t pos;           /**< next frame byte to emit */
    uint8_t fcs_bytes[2]; /**< the FCS as sent, low byte first */
    uint8_t stage;        /**< 0 idle, 1 open flag, 2 body, 3 fcs, 4 close flag */
    bool half_escape;     /**< 0x7D went out, the escaped byte itself is still owed */
    uint8_t owed;         /**< that escaped byte (already XOR 0x20) */
} byte_frame_tx;

/**
 * @brief Set up an encoder.
 * @param[out] e        Encoder.
 * @param[in]  accm     BYTE_FRAME_ACCM_NONE for nd100x and real ND machines,
 *                      BYTE_FRAME_ACCM_ALL_CONTROL for the USB gateways, or any other mask.
 * @param[in]  add_fcs  true when frames come without FCS and the encoder must append it; false
 *                      when the frame already carries one (a gateway forwarding as-is).
 */
void byte_frame_tx_init(byte_frame_tx *e, uint32_t accm, bool add_fcs);

/**
 * @brief Start sending a frame. The frame memory must stay valid until byte_frame_tx_done().
 * @param[in,out] e      Encoder; must be idle (byte_frame_tx_done() true).
 * @param[in]     frame  The frame bytes: address, control, information.
 * @param[in]     len    Their count; at least 1.
 * @return 0, or -1 when busy or the arguments are bad.
 */
int byte_frame_tx_begin(byte_frame_tx *e, const uint8_t *frame, size_t len);

/**
 * @brief Produce as many output bytes as fit. Call again when more room exists.
 * @param[in,out] e    Encoder.
 * @param[out]    out  Output buffer.
 * @param[in]     cap  Its size.
 * @return Bytes written; 0 when the frame is finished (or nothing was begun).
 */
size_t byte_frame_tx_produce(byte_frame_tx *e, uint8_t *out, size_t cap);

/**
 * @brief Whether the encoder has nothing left to emit.
 * @param[in] e  Encoder.
 * @return true when idle.
 */
bool byte_frame_tx_done(const byte_frame_tx *e);

/**
 * @brief One-shot encode of a whole frame into a buffer.
 * @param[in]  frame    Frame bytes (without FCS when add_fcs is true).
 * @param[in]  len      Their count.
 * @param[in]  accm     Escape set, as for byte_frame_tx_init().
 * @param[in]  add_fcs  Append the FCS or not.
 * @param[out] out      Output buffer; byte_frame_encoded_max(len) bytes always suffice.
 * @param[in]  cap      Its size.
 * @return Bytes written, or 0 when the output did not fit or len is 0.
 */
size_t byte_frame_encode_accm(const uint8_t *frame, size_t len, uint32_t accm, bool add_fcs,
                              uint8_t *out, size_t cap);

/* ============================================ short form (HDLCGW, RC422Dev) */

/** Longest frame the short-form decoder accepts, in bytes after un-escaping. */
#define BYTE_FRAME_MAX 1024

/**
 * @brief Byte-stuff one frame for the host: flag, the bytes with 0x7E, 0x7D and every byte
 *        below 0x20 escaped, flag. The FCS is sent as given, not added.
 * @param[in]  frame  Frame bytes, FCS included.
 * @param[in]  len    Number of bytes in frame. 0 (or less) writes just the two flags, as this
 *                    function always has.
 * @param[out] out    Must hold 2 + 2 * len bytes (worst case, every byte escaped).
 * @return Number of bytes written to out.
 */
int byte_frame_encode(const uint8_t *frame, int len, uint8_t *out);

/**
 * @brief Collects frames from the byte stream coming from the host.
 *
 * The engine with the FCS check off over a built-in buffer. Not thread safe; one decoder per
 * receiving task. Read buffer, length and overlong; the rest is private.
 */
typedef struct
{
    uint8_t buffer[BYTE_FRAME_MAX]; /**< the frame being collected / last delivered */
    int length;                     /**< bytes in buffer: of the delivered frame right after
                                         byte_frame_decoder_feed() returned true, else so far */
    uint32_t overlong;              /**< frames dropped for being longer than BYTE_FRAME_MAX;
                                         counts from zero storage, NOT cleared by
                                         byte_frame_decoder_reset(), so a reconnect does
                                         not lose the count */
    byte_frame_rx rx;               /**< the engine; private */
} byte_frame_decoder;

/**
 * @brief Return a decoder to the hunting state. A decoder in zeroed memory (static) also
 *        works without it: the first feed sets it up.
 * @param[in,out] dec  Decoder; must not be NULL.
 */
void byte_frame_decoder_reset(byte_frame_decoder *dec);

/**
 * @brief Feed one byte from the host.
 *
 * A flag both ends a frame and opens the next. Empty frames (two flags in a row) are ignored. A
 * frame longer than BYTE_FRAME_MAX is dropped and counted in dec->overlong; one cut by 0x7D 0x7E
 * is dropped.
 *
 * @param[in,out] dec   Decoder; must not be NULL.
 * @param[in]     data  Byte from the host.
 * @retval true   A complete frame is in dec->buffer, dec->length bytes. Read it before the next
 *                feed; the next data byte overwrites it.
 * @retval false  Nothing complete yet.
 */
bool byte_frame_decoder_feed(byte_frame_decoder *dec, uint8_t data);

/**
 * @brief Whether the decoder is between frames.
 * @param[in] dec  Decoder; must not be NULL.
 * @return true when no frame bytes have been collected since the last flag.
 */
bool byte_frame_decoder_is_idle(const byte_frame_decoder *dec);

#ifdef __cplusplus
}
#endif

#endif /* BYTE_FRAME_H */
