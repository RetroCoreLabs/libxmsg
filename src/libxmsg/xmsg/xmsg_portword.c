/*
 * xmsg_portword.c - the port-word generator.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/xmsg/xmsg_portword.h"

void xmsg_portword_init(XmsgPortwordAlloc *a, uint16_t seed)
{
    a->seed = seed;
}

uint16_t xmsg_portword_next(XmsgPortwordAlloc *a, uint16_t index)
{
    uint8_t draw;
    if (index < 1 || index > XMSG_PORTWORD_MAX_INDEX)
    {
        return 0;
    }
    /* The low seven bits of this generator have full period, so the redraw loop always ends. */
    do
    {
        a->seed =
            (uint16_t)((uint32_t)a->seed * XMSG_PORTWORD_MULTIPLIER + XMSG_PORTWORD_INCREMENT);
        draw = (uint8_t)(a->seed & 0x7Fu);
    } while (draw == 0 || draw == 0x7Fu);
    return (uint16_t)((index << 7) | draw);
}
