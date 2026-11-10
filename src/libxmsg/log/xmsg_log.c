/*
 * xmsg_log.c - per-category runtime levels and a single sink.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/log/xmsg_log.h"

#include <stdarg.h>
#include <stdio.h>

static XmsgLogPort s_sink = {0, 0};
static bool s_sink_set = false;

/* The default lets INFO and worse through on every category; a program narrows or widens it. */
static uint8_t s_levels[XMSG_LOG_CATEGORY_COUNT] = {
    XMSG_LOG_INFO, XMSG_LOG_INFO, XMSG_LOG_INFO, XMSG_LOG_INFO, XMSG_LOG_INFO,
    XMSG_LOG_INFO, XMSG_LOG_INFO, XMSG_LOG_INFO, XMSG_LOG_INFO, XMSG_LOG_INFO,
    XMSG_LOG_INFO,
};

void xmsg_log_set_sink(const XmsgLogPort *sink)
{
    if (sink == NULL || sink->write == NULL)
    {
        s_sink.ctx = NULL;
        s_sink.write = NULL;
        s_sink_set = false;
        return;
    }
    s_sink = *sink;
    s_sink_set = true;
}

void xmsg_log_set_level(XmsgLogCategory category, XmsgLogLevel min_level)
{
    if ((int)category < 0 || (int)category >= (int)XMSG_LOG_CATEGORY_COUNT)
    {
        return;
    }
    s_levels[(int)category] = (uint8_t)min_level;
}

void xmsg_log_set_all_levels(XmsgLogLevel min_level)
{
    for (int i = 0; i < (int)XMSG_LOG_CATEGORY_COUNT; i++)
    {
        s_levels[i] = (uint8_t)min_level;
    }
}

bool xmsg_log_is_enabled(XmsgLogCategory category, XmsgLogLevel level)
{
    if (!s_sink_set)
    {
        return false;
    }
    if (level == XMSG_LOG_ERROR)
    {
        return true;
    }
    if ((int)category < 0 || (int)category >= (int)XMSG_LOG_CATEGORY_COUNT)
    {
        return false;
    }
    return (int)level <= (int)s_levels[(int)category];
}

void xmsg_log_write(XmsgLogCategory category, XmsgLogLevel level, const char *fmt, ...)
{
    char line[XMSG_LOG_LINE_MAX];
    va_list args;
    int n;

    if (!s_sink_set || fmt == NULL)
    {
        return;
    }

    va_start(args, fmt);
    /* vsnprintf always terminates within the buffer; a negative result means a formatting error,
     * in which case a short marker is delivered instead of nothing, so the call is still seen. */
    n = vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    if (n < 0)
    {
        line[0] = '?';
        line[1] = '\0';
    }

    s_sink.write(s_sink.ctx, category, level, line);
}
