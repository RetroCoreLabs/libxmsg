/*
 * term_keyseq.h - gather a function key's whole escape sequence, and tell the lone ESC key
 * (escape) apart from it.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Why: a SINTRAN host reads a function key only when all its bytes come in ONE TAD data letter.
 * MEASURED live 2026-11-10 10:55:07 (terminal type 93, TDV): the telnet client sent the TDV
 * HELP key, 1B 5B 34 36 5F (ESC [ 4 6 _), in one write; the bridge held only the ESC
 * (term_escape.h), sent ESC [ in one BDAT and 4 6 _ in the next, 406 ms later, and D100 rang
 * BEL. The same key typed straight at D100's own terminal port works. This detector holds the
 * whole sequence and hands it on as one unit.
 *
 * What a TDV sends, from the sources:
 *  - RetroTerm TDV2200KeyRegistry.cs (ND-246 keyboard, from the TDV-2200/9 User's Guide
 *    ND-30.003.04): every function key is ESC [ <two digits> _ (HJELP ESC[46_, F1 ESC[50_,
 *    keypad ESC[69_ ..); the cursor and HOME keys are single control bytes (BS, GS, CAN);
 *  - NDInsight Developer/Languages/Application/VTM-KEY-CODES.md: "Every special key on this
 *    terminal sends ESC [ <n> _", measured through VTM; the VT100 forms ESC [ A, ESC [ 5 ~ and
 *    the SS3 forms ESC O P .. belong to other terminal types.
 *
 * The rule (ECMA-48 shapes; the bounds are our choice, not a SINTRAN wire fact):
 *  - ESC [ (CSI): parameter bytes 0x30..0x3F and intermediate bytes 0x20..0x2F, then ONE final
 *    byte 0x40..0x7E ends it. ESC [ 4 6 _ ends on '_'.
 *  - ESC O (SS3): the next byte, 0x40..0x7E, ends it.
 *  - ESC then intermediates 0x20..0x2F: they go on until a final byte 0x30..0x7E.
 *  - ESC then any other byte 0x30..0x7E: those two bytes are the whole sequence.
 *  - ESC ESC: the first ESC is passed on as data; the second is held as a new start.
 *  - a byte that cannot be part of the sequence (a control byte, DEL, 0x80 and up) breaks it:
 *    what was held goes on as data, then that byte is handled on its own.
 *  - a sequence that never ends goes on as data when it reaches TERM_KEYSEQ_MAX bytes, or when
 *    no byte has come for `seq_ms`. Nothing is ever dropped.
 *  - a lone ESC with nothing after it for `esc_ms` is the escape key (TAD ESCA), as before.
 *    esc_ms 0: ESC is never the escape key; a lone ESC goes on as data after `seq_ms`.
 *  - term_keyseq_poll() must be called now and then (the bridge does it every loop) so the
 *    waits run out without waiting for another key.
 *
 * What comes out of one call (TermKeyOut), in this order:
 *  1. `escape`: send the host an escape first;
 *  2. `unit` / `unit_len`: bytes that were held, to be sent together in ONE write and NEVER
 *     echoed; `whole` says the sequence ended on its final byte;
 *  3. `byte` when `has_byte`: one plain typed byte, sent and echoed as usual.
 *
 * Pure logic: bytes and a clock in, decisions out; no I/O. term_escape.h stays for code that
 * uses it (XMSGGW); it holds only the ESC.
 */
#ifndef XMSG_TERM_KEYSEQ_H
#define XMSG_TERM_KEYSEQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** The longest sequence held; reaching it without an end passes it on as data. The longest
 *  TDV key (ESC [ n n _) is 5 bytes. */
#define TERM_KEYSEQ_MAX 16u

/** The default wait for a lone ESC, in milliseconds (the escape key). */
#define TERM_KEYSEQ_ESC_MS 50u

/** The default wait for the next byte of a started sequence, in milliseconds. */
#define TERM_KEYSEQ_SEQ_MS 200u

/** Detector state. */
typedef struct TermKeyseq
{
    uint32_t esc_ms;               /**< lone ESC wait; 0 = ESC is never the escape key */
    uint32_t seq_ms;               /**< wait for the next byte of a sequence */
    uint8_t held[TERM_KEYSEQ_MAX]; /**< the sequence so far; held[0] is ESC */
    uint8_t held_len;              /**< 0: nothing held */
    uint32_t last_ms;              /**< when the last held byte came */
} TermKeyseq;

/** What one call gives back; see the order in the file comment. */
typedef struct TermKeyOut
{
    bool escape;                   /**< send an escape before anything else */
    uint8_t unit[TERM_KEYSEQ_MAX]; /**< held bytes, for ONE write, never echoed */
    uint8_t unit_len;              /**< 0: none */
    bool whole;                    /**< `unit` ended on its final byte (a complete key) */
    bool has_byte;                 /**< `byte` is a plain typed byte */
    uint8_t byte;                  /**< the plain byte */
} TermKeyOut;

/**
 * @brief Start a detector.
 * @param k      Detector.
 * @param esc_ms Lone ESC wait (TERM_KEYSEQ_ESC_MS); 0 = ESC is never the escape key.
 * @param seq_ms Wait for the next byte of a sequence (TERM_KEYSEQ_SEQ_MS); 0 is taken as 1.
 */
void term_keyseq_init(TermKeyseq *k, uint32_t esc_ms, uint32_t seq_ms);

/**
 * @brief Feed one typed byte.
 * @param k      Detector.
 * @param b      The byte.
 * @param now_ms The time it arrived.
 * @param out    Filled (cleared first).
 * @return 0, or -1 for a NULL argument.
 */
int term_keyseq_byte(TermKeyseq *k, uint8_t b, uint32_t now_ms, TermKeyOut *out);

/**
 * @brief Let the waits run out: a lone ESC turns into an escape, an unfinished sequence goes
 *        on as data.
 * @param k      Detector.
 * @param now_ms Current time.
 * @param out    Filled (cleared first); only `escape` or `unit` can be set.
 * @return true when `out` holds something to do.
 */
bool term_keyseq_poll(TermKeyseq *k, uint32_t now_ms, TermKeyOut *out);

/**
 * @brief Whether bytes are held (a sequence or a lone ESC waits).
 * @param k Detector.
 * @return true while something is held.
 */
bool term_keyseq_holding(const TermKeyseq *k);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_TERM_KEYSEQ_H */
