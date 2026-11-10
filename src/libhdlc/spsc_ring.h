/**************************************************************************
** SPSC_RING - lock-free byte ring for ONE writer and ONE reader         **
**                                                                       **
** "SPSC" = single producer, single consumer. Exactly one task, thread   **
** or interrupt handler writes, and exactly one (a different one or the  **
** same) reads. Under that rule no lock is needed: the writer only ever  **
** moves the write index, the reader only the read index, and C11        **
** atomics with release/acquire order make sure the reader never sees an **
** index before the bytes it covers (on the Pico's Cortex-M0+ the        **
** compiler emits the DMB barriers for this).                            **
**                                                                       **
** Use it where ring_buffer cannot be used: on the Raspberry Pi Pico     **
** (RC422Dev's FreeRTOS has mutexes switched off, so ring_buffer has no  **
** lock there - BUGS.md L-PICO) and from an interrupt handler on any     **
** platform. Ronny's choice, 2026-11-09.                                 **
**                                                                       **
** TWO WRITERS (or two readers) AT ONCE CORRUPT IT. Nothing checks this. **
** If more than one task must write, use ring_buffer.                    **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#ifndef SPSC_RING_H
#define SPSC_RING_H

#include <stdbool.h>
#include <stdint.h>

/* The index type. C uses C11 atomic_int; C++ (an Arduino .ino includes this
 * header) has no atomic_int before C++23, so it sees std::atomic<int>, which
 * GCC lays out exactly like C's atomic_int. Only the .c file touches them. */
#ifdef __cplusplus
#include <atomic>
typedef std::atomic<int> spsc_ring_index;
#else
#include <stdatomic.h>
typedef atomic_int spsc_ring_index;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A lock-free byte ring over caller-owned storage.
 *
 * Holds at most size - 1 bytes (one slot stays empty so full and empty can
 * be told apart without a shared counter). All fields are private.
 */
typedef struct
{
    uint8_t *data;              /**< storage, size bytes, owned by the caller */
    int size;                   /**< storage size in bytes */
    spsc_ring_index in_ptr;     /**< next write position; written by the writer only */
    spsc_ring_index out_ptr;    /**< next read position; written by the reader only */
    int high_water;             /**< most bytes ever waiting; written by the writer only */
} spsc_ring;

/**
 * @brief Set up a ring over caller-owned storage.
 *
 * Must run before the writer or the reader starts.
 *
 * @param[out] r        Ring to set up; must not be NULL.
 * @param[in]  storage  At least size bytes; must outlive the ring.
 * @param[in]  size     Storage size, at least 2. The ring holds size - 1 bytes.
 * @return true when ready, false on bad arguments.
 */
bool spsc_ring_init(spsc_ring *r, uint8_t *storage, int size);

/**
 * @brief Writer: append as many of n bytes as fit.
 * @param[in,out] r     Ring; must not be NULL.
 * @param[in]     data  Bytes to append.
 * @param[in]     n     Number of bytes.
 * @return Bytes appended, 0..n.
 */
int spsc_ring_write_some(spsc_ring *r, const uint8_t *data, int n);

/**
 * @brief Writer: append n bytes as one unit, or nothing.
 *
 * The write index moves once, after all n bytes are stored, so the reader
 * sees the whole block or none of it. That keeps a bit-stuffed frame whole
 * on its way to the line (see ring_buffer_write_block for why it matters).
 *
 * @param[in,out] r     Ring; must not be NULL.
 * @param[in]     data  Bytes to append.
 * @param[in]     n     Number of bytes, at most size - 1.
 * @return true when all n bytes were appended, false when they did not fit
 *         (nothing is written then).
 */
bool spsc_ring_write_block(spsc_ring *r, const uint8_t *data, int n);

/**
 * @brief Reader: take up to max of the oldest bytes.
 * @param[in,out] r    Ring; must not be NULL.
 * @param[out]    out  At least max bytes.
 * @param[in]     max  Most bytes to take.
 * @return Bytes taken, 0 when empty.
 */
int spsc_ring_read_block(spsc_ring *r, uint8_t *out, int max);

/**
 * @brief Bytes waiting. Exact for the reader; for anyone else a value that
 *        was true a moment ago.
 * @param[in] r  Ring; must not be NULL.
 * @return 0 .. size - 1.
 */
int spsc_ring_length(const spsc_ring *r);

/**
 * @brief Room for writing. Exact for the writer; for anyone else a value
 *        that was true a moment ago.
 * @param[in] r  Ring; must not be NULL.
 * @return 0 .. size - 1.
 */
int spsc_ring_room(const spsc_ring *r);

/**
 * @brief The most bytes that were ever waiting at once, as seen by the writer.
 * @param[in] r  Ring; must not be NULL.
 * @return 0 .. size - 1.
 */
int spsc_ring_high_water(const spsc_ring *r);

#ifdef __cplusplus
}
#endif

#endif /* SPSC_RING_H */
