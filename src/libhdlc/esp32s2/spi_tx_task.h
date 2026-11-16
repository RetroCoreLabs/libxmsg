/**************************************************************************
** SPI_TX_TASK - transmit half of the synchronous line                   **
**                                                                       **
** This end GENERATES the transmit clock: the SPI2 peripheral runs as a  **
** master, clock out on PIN_TX_CLOCK, data out on PIN_TX_DATA. Bytes to  **
** send are taken from g_tx_buffer; when it is empty the line is kept at **
** HDLC_IDLE (all ones, marking). The receive half, which follows the    **
** far end's clock, is spi_rx_task.                                      **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#ifndef SPI_TX_TASK_H
#define SPI_TX_TASK_H

#include <stdint.h>
#include "libhdlc/ring_buffer.h"

#ifdef __cplusplus
extern "C"
{
#endif

extern ring_buffer g_tx_buffer; /**< bit-stuffed bytes waiting for the line */

/** Where the transmit ring's storage comes from (spi_tx_task_init_ring). */
typedef enum
{
    SPI_TX_RING_INTERNAL = 0, /**< internal RAM: the hot-path rule (hot_path.h) */
    SPI_TX_RING_PSRAM         /**< PSRAM when the board has it, else internal RAM */
} spi_tx_ring_place;

/**
 * @brief Set up g_tx_buffer as 4096 bytes of internal RAM. Call once from setup(), before any
 *        task starts; stops there with an error line when the memory is not there. The same as
 *        spi_tx_task_init_ring(4096, SPI_TX_RING_INTERNAL).
 */
void spi_tx_task_init(void);

/**
 * @brief Set up g_tx_buffer with a size and a place the firmware chooses. Call once from
 *        setup(), before any task starts, INSTEAD of spi_tx_task_init().
 *
 * The ring must hold a whole bit-stuffed frame at once (spi_tx_task_push takes all or none):
 * size - 1 is the largest push. Worst case, every frame byte 0xFF, bit stuffing adds one bit
 * per five, so a 4105-byte frame is 4105 * 8 * 6 / 5 = 39408 bits = 4926 bytes, plus its
 * flags; one such frame needs a ring of at least about 4930 bytes, and 8192 lets the next one
 * be queued while it goes out.
 *
 * SPI_TX_RING_PSRAM goes against the hot-path rule of 2026-11-09 (every byte on the line in
 * internal RAM, hot_path.h): Ronny left the choice to the firmware on 2026-11-16, for builds
 * whose internal RAM has no room. NOT MEASURED: whether the transmit task reading the ring
 * from PSRAM keeps the line fed at 307200 bit/s or 983000 bit/s. The DMA chunks stay in
 * internal RAM either way.
 *
 * @param[in] size   Ring size in bytes, at least 64.
 * @param[in] place  Internal RAM or PSRAM.
 * @return 0, or -1 for a size below 64, a second call, or no memory.
 */
int spi_tx_task_init_ring(int size, spi_tx_ring_place place);

/**
 * @brief The largest block spi_tx_task_push() accepts: the ring size minus one, 0 before the
 *        ring is set up.
 * @return Bytes.
 */
int spi_tx_task_push_max(void);

/**
 * @brief FreeRTOS task entry point. Runs the transmitter forever.
 *
 * Opens SPI2 as master through the ESP-IDF spi_master driver with DMA, keeps
 * two 64-byte chunks queued, and sleeps on the driver until one finishes;
 * then refills it from g_tx_buffer (idle ones when empty) and queues it
 * again. The clock pauses for the driver's overhead between chunks; an HDLC
 * receiver does not mind, it samples on edges. Never returns.
 *
 * @param[in] params  Unused.
 */
void spi_tx_task_run(void *params);

/**
 * @brief Set the transmit bit rate.
 *
 * The SPI clock is the line clock, so bit/s equals SPI Hz. Takes effect at
 * the next chunk. Safe to call from another task.
 *
 * @param[in] bits_per_second  New rate.
 */
void spi_tx_task_set_bit_rate(uint32_t bits_per_second);

/**
 * @brief Current transmit bit rate.
 * @return bit/s.
 */
uint32_t spi_tx_task_bit_rate(void);

/**
 * @brief Run or stop the transmit clock.
 *
 * With the clock stopped the line is quiet: the clock pin sits at its idle
 * level, TD at 1, no DMA runs. Frames pushed meanwhile wait in g_tx_buffer
 * and go out when the clock starts again (the first thing out is whatever
 * was already queued). Takes effect within two chunk times. Safe to call
 * from another task. Added for bench work with a far end whose receiver is
 * sensitive to a continuous clock (the CLOCK console command).
 *
 * @param[in] enabled  true runs the clock (the default), false stops it.
 */
void spi_tx_task_set_clock_enabled(bool enabled);

/**
 * @brief Whether the transmit clock is running.
 * @return true when running.
 */
bool spi_tx_task_clock_enabled(void);

/**
 * @brief Queue raw bytes for the line, all of them or none.
 *
 * The block enters g_tx_buffer in one locked write, so a frame pushed here
 * can never be split by idle bytes on the line. Blocks (1 ms sleeps) until
 * the whole block fits, so it must be called from a task, never from an
 * interrupt.
 *
 * @param[in] bytes  Line bytes, already bit stuffed if they are a frame.
 * @param[in] n      Number of bytes.
 * @return 0 when queued, -1 when n can never fit (larger than the buffer).
 */
int spi_tx_task_push(const uint8_t *bytes, int n);

/**
 * @brief Is there room in g_tx_buffer for n more bytes right now?
 *
 * For a sender that must not sleep in spi_tx_task_push() (the WiFi task:
 * while it sleeps there it reads nothing from the TCP socket and sends
 * nothing to it, BUGS.md T1/T2). Only that task and the console write the
 * ring, so the room can only grow until the caller pushes.
 *
 * @param[in] n  Bytes the caller wants to push.
 * @return true when a push of n bytes will not wait.
 */
bool spi_tx_task_has_room(int n);

/**
 * @brief Bytes taken from g_tx_buffer and clocked onto the line since boot
 *        (frames and the idle byte after each closing flag).
 *
 * Counted when a chunk is filled, so up to two chunks (128 bytes) ahead of
 * the wire.
 *
 * @return Count since boot; wraps at 2^32.
 */
uint32_t spi_tx_task_data_bytes(void);

/**
 * @brief Idle 0xFF fill bytes clocked because g_tx_buffer was empty.
 *
 * With a full window of frames queued on the host, a growing count here
 * means the host-to-line path, not the line, set the rate.
 *
 * @return Count since boot; wraps at 2^32.
 */
uint32_t spi_tx_task_idle_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* SPI_TX_TASK_H */
