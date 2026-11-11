/*
 * fa_envelope.c - the 8-byte head of every FA message, and its counters.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libcosmos/fa/fa_envelope.h"

bool fa_envelope_parse(const uint8_t *body, size_t len, FaEnvelope *e)
{
    if (body == NULL || e == NULL || len < FA_ENVELOPE_SIZE)
    {
        return false;
    }
    e->type = (uint16_t)((body[0] << 8u) | body[1]);
    e->conversation = (uint16_t)((body[2] << 8u) | body[3]);
    e->counter = body[4];
    e->token = (uint16_t)((body[6] << 8u) | body[7]);
    return true;
}

void fa_envelope_write(uint8_t *out, const FaEnvelope *e)
{
    if (out == NULL || e == NULL)
    {
        return;
    }
    out[0] = (uint8_t)(e->type >> 8u);
    out[1] = (uint8_t)(e->type & 0xFFu);
    out[2] = (uint8_t)(e->conversation >> 8u);
    out[3] = (uint8_t)(e->conversation & 0xFFu);
    out[4] = e->counter;
    out[5] = 0;
    out[6] = (uint8_t)(e->token >> 8u);
    out[7] = (uint8_t)(e->token & 0xFFu);
}

uint8_t fa_counter_take(FaCounter *c, bool first_half)
{
    uint8_t v;

    if (c == NULL)
    {
        return 0;
    }
    v = (uint8_t)(c->next & 0x7Fu);
    c->next = (uint8_t)((c->next + 1u) & 0x7Fu);
    return first_half ? v : (uint8_t)(v | 0x80u);
}
