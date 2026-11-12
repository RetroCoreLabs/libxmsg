/*
 * term_report.c - the text of the bridge's WHO and INFO commands.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/term/term_report.h"

#include <stdarg.h>
#include <stdio.h>

/* Append formatted text; on overflow mark the buffer failed. */
typedef struct Out
{
    char *buf;
    size_t cap;
    size_t len;
    bool failed;
} Out;

static void put(Out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void put(Out *o, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (o->failed)
    {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->cap - o->len)
    {
        o->failed = true;
        return;
    }
    o->len += (size_t)n;
}

int term_report_duration(uint32_t s, char *buf, size_t cap)
{
    uint32_t d = s / 86400u;
    uint32_t h = (s / 3600u) % 24u;
    uint32_t m = (s / 60u) % 60u;
    uint32_t sec = s % 60u;
    int n;
    if (buf == NULL || cap == 0)
    {
        return -1;
    }
    if (d > 0u)
    {
        n = snprintf(buf, cap, "%lud %02lu:%02lu:%02lu", (unsigned long)d, (unsigned long)h,
                     (unsigned long)m, (unsigned long)sec);
    }
    else
    {
        n = snprintf(buf, cap, "%02lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m,
                     (unsigned long)sec);
    }
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

int term_report_who(const TermWhoEntry *e, size_t n, char *buf, size_t cap)
{
    Out o = {buf, cap, 0, false};
    if ((e == NULL && n != 0) || buf == NULL || cap == 0)
    {
        return -1;
    }
    buf[0] = '\0';
    put(&o, "  TERM USER             FROM                        CONNECTED    DOING\r\n");
    for (size_t i = 0; i < n; i++)
    {
        char dur[16];
        if (term_report_duration(e[i].connected_s, dur, sizeof dur) < 0)
        {
            return -1;
        }
        put(&o, "%c %4d %-16s %-27s %-12s ", e[i].is_self ? '*' : ' ', e[i].terminal,
            (e[i].user != NULL) ? e[i].user : "(login)", (e[i].remote != NULL) ? e[i].remote : "?",
            dur);
        if (e[i].system == NULL)
        {
            put(&o, "at the prompt\r\n");
        }
        else
        {
            put(&o, "connected to %s (%s)\r\n", e[i].system,
                (e[i].session_state != NULL) ? e[i].session_state : "?");
        }
    }
    return o.failed ? -1 : (int)o.len;
}

int term_report_info(const TermLinkInfo *li, char *buf, size_t cap)
{
    Out o = {buf, cap, 0, false};
    char up[16];
    if (li == NULL || buf == NULL || cap == 0)
    {
        return -1;
    }
    buf[0] = '\0';
    if (term_report_duration(li->uptime_s, up, sizeof up) < 0)
    {
        return -1;
    }
    put(&o, "Local:   node %u, system D%u\r\n", (unsigned)li->own_node, (unsigned)li->own_system);
    put(&o, "Remote:  node %u (%s) configured; ", (unsigned)li->peer_node_cfg,
        (li->peer_system != NULL) ? li->peer_system : "?");
    if (li->peer_node_learned < 0)
    {
        put(&o, "node from the link: unknown\r\n");
    }
    else
    {
        put(&o, "node from the link: %ld\r\n", (long)li->peer_node_learned);
    }
    put(&o, "Link:    %s%s\r\n", li->link_up ? "up" : "down",
        li->link_up ? (li->link_ready ? ", ready" : ", settling") : "");
    put(&o, "Line:    %s\r\n", (li->transport != NULL) ? li->transport : "?");
    if (li->line_bps != 0u)
    {
        put(&o, "Speed:   %lu bits per second\r\n", (unsigned long)li->line_bps);
    }
    else if (li->line_is_tcp)
    {
        put(&o, "Speed:   not known (TCP line)\r\n");
    }
    else
    {
        put(&o, "Speed:   not known\r\n");
    }
    put(&o, "Frames:  %lu in, %lu out, %lu sent again, %lu with an unusual address\r\n",
        (unsigned long)li->frames_in, (unsigned long)li->frames_out,
        (unsigned long)li->retransmissions, (unsigned long)li->unusual_address);
    put(&o, "Window:  the peer's receive window is %u (from its last address byte)\r\n",
        (unsigned)li->peer_window);
    put(&o, "Data:    %lu datagrams in, %lu out\r\n", (unsigned long)li->datagrams_in,
        (unsigned long)li->datagrams_out);
    put(&o, "Uptime:  %s\r\n", up);
    return o.failed ? -1 : (int)o.len;
}
