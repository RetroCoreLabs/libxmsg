/*
 * term_escape.h - tell a lone ESC key (escape) from an ESC that starts a function key sequence.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * A telnet client sends the ESC key as the single byte 0x1B, and a function key as 0x1B followed
 * at once by more bytes (the TDV function key arrives as ESC [ 4 6 _). Both start with the same
 * byte, so the only way to tell them apart is time: an ESC with nothing after it within
 * `timeout_ms` is the ESC key, and the application sends the host an escape (TAD ESCA). An ESC
 * followed in time by any byte is passed on as typed text, unchanged.
 *
 * Pure logic: bytes and a clock in, decisions out; no I/O. Rules:
 *  - timeout 0: ESC is never an escape; every byte passes at once.
 *  - an ESC is held. The next byte, if it comes before the timeout, releases the held ESC and
 *    itself as data. If the timeout has run out by then, the held ESC is an escape first.
 *  - a second ESC right after a held one: the first passes as data, the second is held.
 *  - term_escape_poll() must be called now and then (the bridge does it every loop) so a lone
 *    ESC turns into an escape without waiting for another key.
 */
#ifndef XMSG_TERM_ESCAPE_H
#define XMSG_TERM_ESCAPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** The ESC byte. */
#define TERM_ESC 0x1Bu

/** The default wait for a byte after ESC, in milliseconds. */
#define TERM_ESCAPE_DEFAULT_MS 50u

/** Detector state. */
typedef struct TermEscape
{
    uint32_t timeout_ms; /**< 0 = never an escape */
    bool held;           /**< an ESC is held, waiting to see what follows */
    uint32_t held_at_ms; /**< when it arrived */
} TermEscape;

/**
 * @brief Start a detector.
 * @param e          Detector.
 * @param timeout_ms How long to wait after ESC for another byte; 0 = ESC is always data.
 */
void term_escape_init(TermEscape *e, uint32_t timeout_ms);

/**
 * @brief Feed one typed byte.
 * @param e      Detector.
 * @param b      The byte.
 * @param now_ms The time it arrived.
 * @param out    Receives 0, 1 or 2 data bytes to pass on, in order.
 * @param escape Receives true when an escape must be sent BEFORE the bytes in `out`.
 * @return The number of bytes in `out` (0..2), or -1 for a NULL argument.
 */
int term_escape_byte(TermEscape *e, uint8_t b, uint32_t now_ms, uint8_t out[2], bool *escape);

/**
 * @brief Check whether a held ESC has waited long enough to be the ESC key.
 * @param e      Detector.
 * @param now_ms Current time.
 * @return true when an escape must be sent now (the held ESC is used up); false otherwise.
 */
bool term_escape_poll(TermEscape *e, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_TERM_ESCAPE_H */
