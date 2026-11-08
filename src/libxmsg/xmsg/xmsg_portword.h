/*
 * xmsg_portword.h - minting the port words our sessions are addressed by.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * A port word is `(port index << 7) | r`, where the index is one-based and nine bits wide and r is
 * a 7-bit value from XMSG's own generator, with 0 and 0x7F never used (C# XmsgPortWordAllocator /
 * XmsgRandomGenerator: seed = seed * 5429 + 13849, take the low seven bits, redraw 0 and 127).
 * The random part is what lets a peer tell a fresh port from a stale one at the same index.
 *
 * A PEER's port word is never computed: it is learned from the frames it sends.
 */
#ifndef XMSG_PORTWORD_H
#define XMSG_PORTWORD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define XMSG_PORTWORD_MULTIPLIER 5429u
#define XMSG_PORTWORD_INCREMENT  13849u
#define XMSG_PORTWORD_MAX_INDEX  0x1FFu

/** The generator state. */
typedef struct XmsgPortwordAlloc
{
    uint16_t seed;
} XmsgPortwordAlloc;

/**
 * @brief Seed the generator. Any value; 0 is fine.
 */
void xmsg_portword_init(XmsgPortwordAlloc *a, uint16_t seed);

/**
 * @brief Mint a port word for a port index.
 * @param a     The generator.
 * @param index 1..0x1FF (one-based).
 * @return The port word, or 0 for an index out of range.
 */
uint16_t xmsg_portword_next(XmsgPortwordAlloc *a, uint16_t index);

/** @brief The index half of a port word. */
static inline uint16_t xmsg_portword_index(uint16_t word)
{
    return (uint16_t)(word >> 7);
}

/** @brief The random half of a port word. */
static inline uint8_t xmsg_portword_random(uint16_t word)
{
    return (uint8_t)(word & 0x7Fu);
}

/** @brief Whether the random half is one the generator can produce (1..126). */
static inline bool xmsg_portword_mintable(uint16_t word)
{
    uint8_t r = xmsg_portword_random(word);
    return r >= 1 && r <= 126;
}

#ifdef __cplusplus
}
#endif

#endif /* XMSG_PORTWORD_H */
