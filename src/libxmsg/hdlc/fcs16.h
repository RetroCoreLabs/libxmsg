/*
 * fcs16.h - the HDLC frame check sequence (CRC-16/X-25) used on every ND link frame.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Reflected polynomial 0x8408 (bit reversal of 0x1021), initial value 0xFFFF, the complement of
 * the result sent low byte first. Running the same CRC over a received frame INCLUDING its two FCS
 * bytes leaves the constant 0xF0B8 in the register when the frame is good; that is the check the
 * receiver does, and it is what every ND capture in the RetroIO repository passes. The C#
 * reference is RetroIO.Protocols.Xmsg.Hdlc Fcs16.cs; the anchor vector is the SABM
 * `01 3F 00 64` whose FCS is 0x092E, sent as `2E 09`.
 *
 * Table driven (256 entries, 512 bytes of read-only data) because this runs on every byte of
 * every frame on a Cortex-M0+.
 */
#ifndef XMSG_FCS16_H
#define XMSG_FCS16_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define FCS16_INIT 0xFFFFu /**< register value before the first byte */
#define FCS16_GOOD 0xF0B8u /**< register value after a good frame plus its FCS */
#define FCS16_SIZE 2u      /**< bytes of FCS on the wire */

/**
 * @brief Fold more bytes into a running CRC register.
 * @param fcs  The register so far; FCS16_INIT for the first call.
 * @param data Bytes to fold; may be NULL when len is 0.
 * @param len  Number of bytes.
 * @return The updated register.
 */
uint16_t fcs16_update(uint16_t fcs, const uint8_t *data, size_t len);

/**
 * @brief The CRC register after folding a whole buffer from FCS16_INIT.
 * @param data Bytes to fold.
 * @param len  Number of bytes.
 * @return The register (NOT yet complemented; see fcs16_append()).
 */
uint16_t fcs16_compute(const uint8_t *data, size_t len);

/**
 * @brief Write the two FCS bytes for a register value: the complement, low byte first.
 * @param out Receives FCS16_SIZE bytes.
 * @param fcs The register returned by fcs16_compute() or fcs16_update().
 */
void fcs16_append(uint8_t *out, uint16_t fcs);

/**
 * @brief Check a received frame that still carries its FCS.
 * @param frame_with_fcs The frame bytes including the trailing two FCS bytes.
 * @param len            Its length; anything shorter than 4 (address, control, FCS) is bad.
 * @return true when the register over the whole frame equals FCS16_GOOD.
 */
bool fcs16_is_good(const uint8_t *frame_with_fcs, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_FCS16_H */
