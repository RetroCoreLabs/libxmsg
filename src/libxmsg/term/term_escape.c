/*
 * term_escape.c - tell a lone ESC key from an ESC that starts a function key sequence.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/term/term_escape.h"

#include <string.h>

void term_escape_init(TermEscape *e, uint32_t timeout_ms)
{
    if (e == NULL)
    {
        return;
    }
    memset(e, 0, sizeof *e);
    e->timeout_ms = timeout_ms;
}

int term_escape_byte(TermEscape *e, uint8_t b, uint32_t now_ms, uint8_t out[2], bool *escape)
{
    int n = 0;

    if (e == NULL || out == NULL || escape == NULL)
    {
        return -1;
    }
    *escape = false;
    if (e->held)
    {
        e->held = false;
        if ((uint32_t)(now_ms - e->held_at_ms) >= e->timeout_ms)
        {
            *escape = true; /* the wait ran out before this byte: the held ESC was the key */
        }
        else
        {
            out[n++] = (uint8_t)TERM_ESC; /* a sequence: the ESC goes on as typed */
        }
    }
    if (b == TERM_ESC && e->timeout_ms != 0u)
    {
        e->held = true;
        e->held_at_ms = now_ms;
        return n;
    }
    out[n++] = b;
    return n;
}

bool term_escape_poll(TermEscape *e, uint32_t now_ms)
{
    if (e == NULL || !e->held)
    {
        return false;
    }
    if ((uint32_t)(now_ms - e->held_at_ms) < e->timeout_ms)
    {
        return false;
    }
    e->held = false;
    return true;
}
