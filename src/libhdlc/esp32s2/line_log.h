/**************************************************************************
** LINE_LOG - where the ESP32-S2 line tasks send their text              **
**                                                                       **
** The line tasks have two kinds of text: an error when a driver call    **
** fails (rare, but the line is dead after it), and the DMA ring dump of **
** spi_rx_task_print_dma() for a bench console. The library has no       **
** console of its own, so the application hands it one function that     **
** takes a finished line of text. HDLCGW sends errors as ERROR log       **
** frames and the dump as command reply text.                            **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#ifndef LINE_LOG_H
#define LINE_LOG_H

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief Receives one piece of finished text; no newline is added. */
typedef void (*line_text_fn)(const char *text);

/**
 * @brief Set where driver errors from the line tasks go.
 *
 * Call before the tasks start. NULL (the default) drops them.
 *
 * @param[in] fn  Error sink, or NULL.
 */
void line_log_set_error(line_text_fn fn);

/**
 * @brief Format an error and hand it to the error sink, if one is set.
 *
 * For the line tasks only. Messages longer than 159 characters are cut.
 *
 * @param[in] fmt  printf format, a literal.
 */
void line_log_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/**
 * @brief Format text and hand it to the given sink. Cut at 159 characters.
 * @param[in] fn   Sink; nothing happens when NULL.
 * @param[in] fmt  printf format, a literal.
 */
void line_log_printf(line_text_fn fn, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif

#endif /* LINE_LOG_H */
