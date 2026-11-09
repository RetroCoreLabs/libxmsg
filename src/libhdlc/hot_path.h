/**************************************************************************
** HOT_PATH - place per-byte code and data in internal RAM               **
**                                                                       **
** Why: on the ESP32-S2 code runs from flash and constant tables are     **
** read from flash, both through the cache, and the ring buffers were in **
** PSRAM, also behind the cache. Every byte on the line passes through   **
** a handful of small functions (ring buffer, bit stuffing, CRC, byte    **
** stuffing), so those and the data they touch per byte live in the      **
** chip's internal SRAM instead, where a cache miss cannot stall them    **
** (Ronny's ruling 2026-11-09: hot path in internal RAM, not PSRAM or    **
** flash, as far as memory allows).                                      **
**                                                                       **
** On the Pico only code moves to RAM. On Linux, Windows and in the unit **
** tests the two macros expand to nothing.                               **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen       **
***************************************************************************/
#ifndef HOT_PATH_H
#define HOT_PATH_H

#ifdef ESP_PLATFORM
#include <esp_attr.h>
/** Put a function in internal instruction RAM (IRAM), and never fold it
 *  into a caller: an inlined copy lands wherever the caller lives, which may
 *  be flash (seen 2026-11-09: fill_chunk inlined into queue_chunk ran from
 *  flash although it was marked). */
#define HOT_PATH_CODE IRAM_ATTR __attribute__((noinline))
/** Put a table or variable in internal data RAM, also when it is const. */
#define HOT_PATH_DATA DRAM_ATTR
#elif defined(PICO_ON_DEVICE)
#include <pico/platform.h>
/** Raspberry Pi Pico: run from RAM, not from flash through the XIP cache.
 *  __not_in_flash(group) is the Pico SDK section attribute (pico/platform.h,
 *  checked in SDK 1.5.1). Same no-inline reason as above. A build that runs
 *  everything from RAM anyway (copy_to_ram, as RC422Dev does) loses nothing. */
#define HOT_PATH_CODE __not_in_flash("libhdlc") __attribute__((noinline))
/** Tables stay in flash on the Pico: no RAM section for constant data has
 *  been checked there yet. */
#define HOT_PATH_DATA
#else
#define HOT_PATH_CODE
#define HOT_PATH_DATA
#endif

#endif /* HOT_PATH_H */
