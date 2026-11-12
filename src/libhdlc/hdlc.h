/**************************************************************************
** HDLC - bit-level HDLC/SDLC framing                                    **
**                                                                       **
** Bit stuffing on transmit; bit un-stuffing, flag and abort detection   **
** on receive; CRC-16/IBM-SDLC frame check. Nothing here depends on      **
** Arduino or FreeRTOS, so it can be unit tested on a PC.                **
**                                                                       **
** Wire convention (same as the SPI link and as Mattis Lind's            **
** SDLCBridge): bits go out LSB first. A byte handed to the encoder is   **
** walked from bit 0 to bit 7; the output byte is built by shifting      **
** RIGHT and setting bit 7, so the first bit put on the wire ends up in  **
** bit 0 of the output byte. The SPI peripheral must run LSBFIRST.       **
**                                                                       **
** Logic taken from SDLCBridge.ino (MattisLind/alfaskop_emu,             **
** Utils/SDLCBridge), proven against a real IBM SDLC terminal (Informer  **
** 213) and an Intel 8274 board. Two details from that work are kept on  **
** purpose:                                                              **
**  - a stuffed zero IS inserted when five ones precede the closing      **
**    flag, although the flag itself starts with a zero. Without it the  **
**    terminal rejected every frame whose last FCS byte ended in 11111.  **
**  - after a flag the receive bit counter is cleared, so byte alignment **
**    on the wire does not matter. Frames may start on any bit of an SPI **
**    byte.                                                              **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen       **
***************************************************************************/
#ifndef HDLC_H
#define HDLC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @name CRC-16/IBM-SDLC (also called CRC-16/X-25)
 * Poly 0x1021 reflected (0x8408), init 0xFFFF, bits fed LSB first, result
 * inverted before sending. Running the CRC over a good frame INCLUDING its
 * two FCS bytes leaves HDLC_CRC_GOOD in the register.
 * @{
 */
#define HDLC_CRC_INIT 0xFFFFu /**< starting value of the CRC register */
#define HDLC_CRC_GOOD 0xF0B8u /**< residue after a good frame plus FCS */
/** @} */

#define HDLC_FLAG  0x7Eu /**< 01111110, frame delimiter */
#define HDLC_ABORT 0x7Fu /**< 01111111, seven ones */
#define HDLC_IDLE  0xFFu /**< marking line, nothing sent */

/**
 * @brief One CRC step over one byte.
 * @param[in] crc   Current CRC register.
 * @param[in] data  Next frame byte.
 * @return Updated CRC register.
 */
uint16_t hdlc_crc_byte(uint16_t crc, uint8_t data);

/** Bytes of FCS on the wire. */
#define HDLC_FCS_SIZE 2u

/**
 * @brief The CRC over a buffer, continuing from a register value.
 * @param[in] crc   Register: HDLC_CRC_INIT to start, or a previous result.
 * @param[in] data  Bytes; may be NULL when len is 0.
 * @param[in] len   Number of bytes.
 * @return Updated register.
 */
uint16_t hdlc_fcs_update(uint16_t crc, const uint8_t *data, size_t len);

/**
 * @brief The CRC register after a whole buffer, from HDLC_CRC_INIT.
 *        Pass it to hdlc_fcs_append() to get the two FCS bytes.
 * @param[in] data  Bytes.
 * @param[in] len   Number of bytes.
 * @return Register value (not yet inverted).
 */
uint16_t hdlc_fcs_compute(const uint8_t *data, size_t len);

/**
 * @brief Write the FCS for a register value: the inverted register, low byte first.
 * @param[out] out  Two bytes.
 * @param[in]  crc  Register from hdlc_fcs_compute().
 */
void hdlc_fcs_append(uint8_t *out, uint16_t crc);

/**
 * @brief Whether a frame that ends in its two FCS bytes is good.
 *
 * Fewer than 4 bytes (address, control and the FCS) is never a frame (ITU-T
 * X.25 clause 2.3.5.3 a) and answers false.
 *
 * @param[in] frame_with_fcs  Frame, FCS last.
 * @param[in] len             Its length.
 * @return true when the residue is HDLC_CRC_GOOD.
 */
bool hdlc_fcs_is_good(const uint8_t *frame_with_fcs, size_t len);

/* ------------------------------------------------------------ encoder */

/**
 * @brief Turns frame bytes into the bit-stuffed byte stream for the line.
 *
 * Use: hdlc_encoder_open() once, hdlc_encoder_write() for every byte of
 * address, control, data and FCS, then hdlc_encoder_close(). Every call
 * fills the caller's output buffer and returns how many bytes it produced:
 * 0, 1 or 2, never more, because one input byte adds at most 8 data bits
 * plus 2 stuffed zeros. The caller pushes those bytes to the transmit ring
 * buffer. Not thread safe; one encoder per sending task.
 */
typedef struct
{
    uint8_t shifter; /**< output byte being built, filled from bit 7 down */
    int bit_count;   /**< bits in shifter, 0..7 */
    int one_count;   /**< ones in a row sent so far, 0..5 */
    uint16_t crc;    /**< running CRC over the bytes written */
} hdlc_encoder;

/**
 * @brief Return an encoder to the idle state. Pending bits are discarded.
 * @param[out] enc  Encoder; must not be NULL.
 */
void hdlc_encoder_reset(hdlc_encoder *enc);

/**
 * @brief Write the opening flag and start a frame.
 *
 * The flag is always byte aligned because close/abort reset the bit
 * counter. Also restarts the running CRC.
 *
 * @param[in,out] enc  Encoder; must not be NULL.
 * @param[out]    out  Receives the line bytes; room for 2.
 * @return Number of bytes written to out (always 1).
 */
int hdlc_encoder_open(hdlc_encoder *enc, uint8_t *out);

/**
 * @brief Write one frame byte, bit stuffed. Updates the running CRC.
 * @param[in,out] enc   Encoder; must not be NULL.
 * @param[in]     data  Frame byte (address, control, data or FCS).
 * @param[out]    out   Receives the line bytes; room for 2.
 * @return Number of bytes written to out, 0..2.
 */
int hdlc_encoder_write(hdlc_encoder *enc, uint8_t data, uint8_t *out);

/**
 * @brief Write the two FCS bytes for everything written since open.
 *
 * The FCS is the inverted CRC, low byte first. Use only when the frame does
 * not already carry an FCS.
 *
 * @param[in,out] enc  Encoder; must not be NULL.
 * @param[out]    out  Receives the line bytes; room for 4.
 * @return Number of bytes written to out, 2..4.
 */
int hdlc_encoder_write_fcs(hdlc_encoder *enc, uint8_t *out);

/**
 * @brief Write the closing flag and end the frame.
 *
 * A stuffed zero is inserted first if five ones are pending. Residual bits
 * are followed by the flag bits and then padded with idle ones to a whole
 * byte. Leaves the encoder reset, so the next open is byte aligned.
 *
 * @param[in,out] enc  Encoder; must not be NULL.
 * @param[out]    out  Receives the line bytes; room for 2.
 * @return Number of bytes written to out, 1..2.
 */
int hdlc_encoder_close(hdlc_encoder *enc, uint8_t *out);

/**
 * @brief Write an abort sequence (seven ones) instead of a closing flag.
 * @param[in,out] enc  Encoder; must not be NULL.
 * @param[out]    out  Receives the line bytes; room for 2.
 * @return Number of bytes written to out, 1..2.
 */
int hdlc_encoder_abort(hdlc_encoder *enc, uint8_t *out);

/* ------------------------------------------- encoder, bit-group output */

/*
 * The same encoder, but each call returns a group of BITS instead of packed
 * line bytes: for a transmitter that takes a bit count with every word, such
 * as a Raspberry Pi Pico PIO program. Bit 0 of the group goes on the wire
 * first. A data byte becomes 8, 9 or 10 bits (8 plus the stuffed zeros); a
 * flag or an abort is 8 bits, or 9 when a stuffed zero comes first. No idle
 * padding is ever added, because nothing has to fill out a byte.
 * From RC422Dev src/sync.cpp sendHdlcByte(), measured clean Pico -> ESP32 at
 * 983000 bit/s (7783 of 7783 626-byte frames, 2026-11-09).
 *
 * Use either the byte form above or this form for a frame, never both: the
 * byte form keeps unsent bits in the encoder, this form never does.
 */

/**
 * @brief Start a frame: the opening flag as 8 bits. Restarts the CRC.
 * @param[in,out] enc   Encoder; must not be NULL.
 * @param[out]    bits  Receives the flag, bit 0 first on the wire.
 * @return Number of bits in *bits (always 8).
 */
int hdlc_encoder_open_bits(hdlc_encoder *enc, uint32_t *bits);

/**
 * @brief One frame byte as stuffed bits. Updates the running CRC.
 * @param[in,out] enc   Encoder; must not be NULL.
 * @param[in]     data  Frame byte (address, control, data or FCS).
 * @param[out]    bits  Receives the bits, bit 0 first on the wire.
 * @return Number of bits in *bits, 8..10.
 */
int hdlc_encoder_write_bits(hdlc_encoder *enc, uint8_t data, uint32_t *bits);

/**
 * @brief The FCS for everything written since open: the inverted CRC.
 *
 * Send it low byte first through hdlc_encoder_write_bits(). Only for frames
 * that do not already carry their FCS.
 *
 * @param[in] enc  Encoder; must not be NULL.
 * @return The two FCS bytes, low byte = first on the wire.
 */
uint16_t hdlc_encoder_fcs(const hdlc_encoder *enc);

/**
 * @brief End the frame: a stuffed zero if five ones are pending, then the
 *        closing flag. Leaves the encoder reset.
 * @param[in,out] enc   Encoder; must not be NULL.
 * @param[out]    bits  Receives the bits, bit 0 first on the wire.
 * @return Number of bits in *bits, 8..9.
 */
int hdlc_encoder_close_bits(hdlc_encoder *enc, uint32_t *bits);

/**
 * @brief End the frame with an abort (seven ones) instead of a flag.
 * @param[in,out] enc   Encoder; must not be NULL.
 * @param[out]    bits  Receives the bits, bit 0 first on the wire.
 * @return Number of bits in *bits, 8..9.
 */
int hdlc_encoder_abort_bits(hdlc_encoder *enc, uint32_t *bits);

/* ------------------------------------------------------------ decoder */

/**
 * @brief How a frame ended at its closing flag. Handed to on_end().
 *
 * Checked in this order, the first that applies wins.
 */
typedef enum
{
    HDLC_RX_OK = 0,   /**< whole bytes, CRC residue HDLC_CRC_GOOD */
    HDLC_RX_TOO_LONG, /**< more bytes than hdlc_decoder_set_max_bytes() allows; the
                             extra bytes were NOT passed to on_byte() */
    HDLC_RX_ODD_BITS, /**< the closing flag did not fall on a byte boundary: the bits
                             between the flags are not a whole number of bytes (ISO 13239
                             4.3.3 invalid frame; usually a bit slip). The last byte passed
                             to on_byte() holds some of the flag's own bits. */
    HDLC_RX_BAD_CRC   /**< whole bytes, but the CRC residue is wrong */
} hdlc_rx_status;

/**
 * @brief Where hdlc_decoder_feed() delivers frames.
 *
 * The callbacks run on the caller's task, inside hdlc_decoder_feed().
 * ctx is passed back unchanged.
 */
typedef struct
{
    /** Handed to every callback unchanged. */
    void *ctx;
    /** Per-byte mode (no buffer set): one un-stuffed frame byte, in order.
     *  The FCS bytes come last. Not called in per-frame mode, may be NULL
     *  there. */
    void (*on_byte)(void *ctx, uint8_t data);
    /** Per-byte mode: closing flag after at least HDLC_MIN_FRAME_BYTES
     *  bytes. status says whether the frame is good; anything but
     *  HDLC_RX_OK means drop it. Not called in per-frame mode, may be NULL
     *  there. */
    void (*on_end)(void *ctx, hdlc_rx_status status);
    /** Seven or more ones inside a frame; the frame is dropped. */
    void (*on_abort)(void *ctx);
    /** The bytes delivered since the last flag were fewer than
     *  HDLC_MIN_FRAME_BYTES and are not a frame: forget them, count nothing.
     *  Without this the sink kept a padding byte and the next frame arrived
     *  with it stuck on the front (bench 2026-11-08, 91 of 155 frames). */
    void (*on_discard)(void *ctx);
    /** Per-frame mode (a buffer set with hdlc_decoder_set_buffer()): closing
     *  flag after at least HDLC_MIN_FRAME_BYTES bytes. The frame is in the
     *  buffer, length bytes from the start, FCS included as its last two
     *  bytes; for HDLC_RX_TOO_LONG length is the buffer size and the rest
     *  of the frame was not kept. Same status meaning as on_end. The buffer
     *  stays the decoder's until this returns: give it the next one here
     *  (or right after) with hdlc_decoder_set_buffer(), or it reuses this
     *  one. Not called in per-byte mode, may be NULL there. */
    void (*on_frame)(void *ctx, int length, hdlc_rx_status status);
} hdlc_frame_sink;

/**
 * @brief Counters kept by a decoder. Cleared only by hdlc_decoder_reset().
 *
 * Every frame that reaches a closing flag or an abort is counted in exactly
 * one of the first six fields.
 */
typedef struct
{
    uint32_t ok;           /**< frames ended HDLC_RX_OK */
    uint32_t bad_crc;      /**< frames ended HDLC_RX_BAD_CRC */
    uint32_t odd_bits;     /**< frames ended HDLC_RX_ODD_BITS */
    uint32_t too_long;     /**< frames ended HDLC_RX_TOO_LONG */
    uint32_t aborts;       /**< frames of HDLC_MIN_FRAME_BYTES or more cut by seven ones */
    uint32_t short_frames; /**< flag-to-flag runs of 1..3 bytes, ignored (see feed) */
    uint32_t bytes;        /**< bytes passed on: to on_byte(), or stored in the buffer */
} hdlc_rx_stats;

/**
 * @brief Pulls frames out of the raw byte stream read from the line.
 *
 * Not thread safe; one decoder per receiving task. All fields are private;
 * read the counters through stats.
 */
typedef struct
{
    bool active;         /**< opening flag seen, collecting bits */
    bool have_data;      /**< at least one byte delivered for this frame */
    bool overflow;       /**< this frame passed max_bytes */
    int byte_count;      /**< bytes assembled for this frame, kept or not */
    int max_bytes;       /**< longest frame passed on, FCS included; 0 = no limit */
    uint8_t *buf;        /**< per-frame mode: where frame bytes go; NULL = per-byte mode */
    int buf_size;        /**< size of buf in bytes */
    int limit;           /**< bytes kept per frame: the smaller of max_bytes and
                             buf_size, 0 = none; worked out once, used per byte */
    uint8_t shifter;     /**< input byte being built, filled from bit 7 down */
    int bit_count;       /**< bits in shifter, 0..7 */
    int one_count;       /**< ones in a row seen so far, capped at 8 */
    uint16_t crc;        /**< running CRC over the bytes delivered */
    hdlc_rx_stats stats; /**< counters, see hdlc_rx_stats */
} hdlc_decoder;

/** Shortest run between two flags that counts as a frame: address, control
 *  and FCS = 32 bits (ISO 13239 4.3.3, "invalid frame" rule; SDLC says the
 *  same). Anything shorter is ignored, not counted as a bad frame. */
#define HDLC_MIN_FRAME_BYTES 4

/**
 * @brief Return a decoder to the idle, waiting-for-flag state, clear its
 *        counters, remove any length limit and any buffer (back to
 *        per-byte mode).
 *
 * The line before the reset is taken to have been idle (all ones), so the
 * first flag is only recognised after a zero has been seen. Starting the
 * count at zero instead let six ones that were really the tail of a longer
 * run look like a flag (RC422Dev, measured 2026-11-08).
 *
 * @param[out] dec  Decoder; must not be NULL.
 */
void hdlc_decoder_reset(hdlc_decoder *dec);

/**
 * @brief Limit how many bytes of one frame are passed on.
 *
 * Bytes past the limit are not handed to on_byte() and the frame ends with
 * HDLC_RX_TOO_LONG. Call after hdlc_decoder_reset(). For ND LAPB with
 * N1 = 622 the longest frame is 626 bytes (address, control, 622, FCS).
 *
 * @param[in,out] dec        Decoder; must not be NULL.
 * @param[in]     max_bytes  Longest frame in bytes, FCS included; 0 = no limit.
 */
void hdlc_decoder_set_max_bytes(hdlc_decoder *dec, int max_bytes);

/**
 * @brief Switch to per-frame mode: frame bytes go straight into buf, and the
 *        sink hears once per frame (on_frame) instead of once per byte.
 *
 * At 983000 bit/s the per-byte callback runs up to about 123000 times a
 * second; in this mode a whole data byte costs a table lookup, a CRC step and
 * one store. Asked for by RC422Dev, 2026-11-09.
 *
 * Never writes past size bytes: a longer frame ends HDLC_RX_TOO_LONG, so size
 * also works as a length limit (the smaller of size and the max_bytes limit
 * wins). May be called at any time, also from inside on_frame, on_abort and
 * on_discard to hand over the next buffer; a frame already in progress keeps
 * its bytes in the old buffer and continues in the new one at the same
 * offset, so swap only at a frame boundary (inside those callbacks).
 * buf = NULL or size <= 0 goes back to per-byte mode, exactly as before.
 *
 * @param[in,out] dec   Decoder; must not be NULL.
 * @param[in]     buf   Frame buffer, or NULL; must outlive its use.
 * @param[in]     size  Bytes in buf.
 */
void hdlc_decoder_set_buffer(hdlc_decoder *dec, uint8_t *buf, int size);

/**
 * @brief Feed one byte as read from the line (LSB first on the wire).
 *
 * Idle bytes (0xFF) outside a frame return at once. Inside a frame a byte
 * with no stuffed zero, flag or abort in it is taken whole (a table lookup,
 * the fast path RC422Dev measured at 983000 bit/s: decode CPU 75% and 30%
 * of 626-byte frames lost before, none lost after); any other byte is walked
 * bit by bit. Callbacks fire from here.
 *
 * Runs of fewer than HDLC_MIN_FRAME_BYTES between two flags are dropped in
 * silence and counted in stats.short_frames. They happen whenever one frame
 * follows another directly: the transmitter pads the byte after the closing
 * flag with 1..7 idle ones, and those plus the next opening flag make 8 or
 * more bits, which this decoder would otherwise deliver as a one-byte frame
 * with a bad FCS. Seen on the bench 2026-11-08: 1339 such phantoms in 5521
 * real frames, every real frame good.
 *
 * @param[in,out] dec   Decoder; must not be NULL.
 * @param[in]     data  Byte from the line.
 * @param[in]     sink  Callbacks for frame bytes and frame ends; must not be
 *                      NULL. on_abort and on_discard must be set; on_byte and
 *                      on_end in per-byte mode, on_frame in per-frame mode.
 */
void hdlc_decoder_feed(hdlc_decoder *dec, uint8_t data, const hdlc_frame_sink *sink);

/**
 * @brief Name of a receive status, for logs and consoles.
 * @param[in] status  Status.
 * @return "OK", "TOO_LONG", "ODD_BITS", "BAD_CRC" or "?".
 */
const char *hdlc_rx_status_name(hdlc_rx_status status);

/**
 * @brief The whole-byte table used by the receive fast path (see hdlc.c).
 *
 * Exposed only so the unit tests can rebuild it from its rule and compare.
 *
 * @return 256 entries.
 */
const uint8_t *hdlc_run_table(void);

/* ------------------------------------------------------ control field */

/** @brief The three HDLC frame classes, told apart by the low control bits. */
typedef enum
{
    HDLC_FRAME_UNKNOWN, /**< could not classify (never happens for 8-bit control) */
    HDLC_FRAME_I,       /**< information: bit 0 = 0, carries N(S) and N(R) */
    HDLC_FRAME_S,       /**< supervisory: bits 1..0 = 01, carries N(R) */
    HDLC_FRAME_U        /**< unnumbered: bits 1..0 = 11, M1 and M2 name the type */
} hdlc_frame_type;

/**
 * @brief A decoded 8-bit (modulo 8) control field, as the DECODE, F and
 *        INSPECT console commands print it. Same layout and names as the
 *        RC422Dev console so both gateways read alike.
 */
typedef struct
{
    hdlc_frame_type type;    /**< I, S or U */
    int ns;                  /**< N(S), I frames only, bits 3..1 */
    int nr;                  /**< N(R), I and S frames, bits 7..5 */
    int m1;                  /**< U frames: bits 3..2 */
    int m2;                  /**< U frames: bits 7..5 */
    bool poll_final;         /**< bit 4, P/F */
    const char *name;        /**< short type name: RR, SABM, UA, ... "" for I */
    const char *description; /**< one-line meaning of name */
} hdlc_control_field;

/**
 * @brief Decode a control byte (modulo 8 numbering, ISO 13239 5.4).
 *
 * U-frame types are found by M1/M2 in a table shared with RC422Dev; the
 * first match wins where a command and a response share the bits (UI, XID,
 * TEST, DM/SARM). Unknown M1/M2 pairs decode as "UNKNOWN".
 *
 * @param[in] control  The control byte, normally the second byte of a frame.
 * @return The decoded fields; name and description point at static strings.
 */
hdlc_control_field hdlc_decode_control(uint8_t control);

/**
 * @brief One-letter class name.
 * @param[in] type  Frame class.
 * @return "I", "S", "U" or "?".
 */
const char *hdlc_frame_type_name(hdlc_frame_type type);

/**
 * @brief Long class name.
 * @param[in] type  Frame class.
 * @return "Information Frame", "Supervisory Frame", "Unnumbered Frame" or
 *         "Unknown Frame".
 */
const char *hdlc_frame_type_description(hdlc_frame_type type);

#ifdef __cplusplus
}
#endif

#endif /* HDLC_H */
