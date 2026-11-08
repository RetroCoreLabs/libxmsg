/*
 * xmsg_log.h - the one diagnostics channel of the library.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * XMSG_LOG(category, level, fmt, ...) is the only way library code reports anything (house rule
 * 9). The macro tests the category's runtime level BEFORE any argument is evaluated, so a disabled
 * category costs one table lookup and no formatting. A compile-time ceiling, XMSG_LOG_MAX_LEVEL,
 * removes the format strings of higher levels from the binary altogether, which matters on a
 * Pico.
 *
 * The sink is the application's (xmsg_ports.h): the library hands it a finished line with no
 * newline, no timestamp and no category tag, and the sink decides what to do - Serial on an
 * ESP32, stderr in a test, a ring buffer on a device with no console. One sink per program: this
 * is deliberate, so that a node, a session and a helper all land in the same place without each
 * carrying a pointer to it.
 *
 * Guest text - terminal output, file contents - never passes through here (house rule 9.5).
 */
#ifndef XMSG_LOG_H
#define XMSG_LOG_H

#include <stdbool.h>

#include "libxmsg/xmsg_config.h"
#include "libxmsg/xmsg_ports.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Longest line the formatter produces, including the terminating NUL. Longer lines are cut. */
#define XMSG_LOG_LINE_MAX 128

/**
 * @brief Install the sink every log line goes to. NULL (the default) means silence.
 * @param sink Copied; may be NULL.
 */
void xmsg_log_set_sink(const XmsgLogPort *sink);

/**
 * @brief Set the lowest level a category reports. ERROR is always reported regardless.
 * @param category Which subsystem.
 * @param min_level Lines at this level or below (more severe) pass. XMSG_LOG_ERROR silences all
 *                  but errors; XMSG_LOG_TRACE lets everything through.
 */
void xmsg_log_set_level(XmsgLogCategory category, XmsgLogLevel min_level);

/**
 * @brief Set the same minimum level for every category.
 * @param min_level As for xmsg_log_set_level().
 */
void xmsg_log_set_all_levels(XmsgLogLevel min_level);

/**
 * @brief Whether a line at this category and level would reach the sink.
 * @param category Which subsystem.
 * @param level    Severity of the line.
 * @return true when there is a sink and the level passes the category's threshold.
 */
bool xmsg_log_is_enabled(XmsgLogCategory category, XmsgLogLevel level);

/**
 * @brief Format and deliver one line. Call through XMSG_LOG, not directly.
 * @param category Which subsystem.
 * @param level    Severity.
 * @param fmt      printf format; the result is cut at XMSG_LOG_LINE_MAX - 1 characters.
 */
void xmsg_log_write(XmsgLogCategory category, XmsgLogLevel level, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

/**
 * @brief Report a line. Arguments are not evaluated unless the line will be delivered.
 *
 * `level` must be a constant so the compile-time ceiling can remove the whole statement.
 */
#define XMSG_LOG(category, level, ...)                                                             \
    do                                                                                             \
    {                                                                                              \
        if ((int)(level) <= XMSG_LOG_MAX_LEVEL && xmsg_log_is_enabled((category), (level)))        \
        {                                                                                          \
            xmsg_log_write((category), (level), __VA_ARGS__);                                      \
        }                                                                                          \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* XMSG_LOG_H */
