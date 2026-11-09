/**************************************************************************
** LINE_LOG - where the ESP32-S2 line tasks send their text              **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#ifdef ESP_PLATFORM
#include <sdkconfig.h>
#endif
/* The ESP32-S2 line code only: everywhere else this file is empty. */
#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_IDF_TARGET_ESP32S2)

#include "libhdlc/esp32s2/line_log.h"

#include <stdarg.h>
#include <stdio.h>

static line_text_fn s_error = NULL;

void line_log_set_error(line_text_fn fn)
{
    s_error = fn;
}

/**
 * @brief Format into a local buffer and hand it on.
 * @param[in] fn   Sink, may be NULL.
 * @param[in] fmt  Format.
 * @param[in] ap   Arguments.
 */
static void emit(line_text_fn fn, const char *fmt, va_list ap)
{
    char text[160];
    if (fn == NULL)
    {
        return;
    }
    if (vsnprintf(text, sizeof text, fmt, ap) < 0)
    {
        return;
    }
    fn(text);
}

void line_log_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit(s_error, fmt, ap);
    va_end(ap);
}

void line_log_printf(line_text_fn fn, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit(fn, fmt, ap);
    va_end(ap);
}

#endif /* ESP32-S2 Arduino */
