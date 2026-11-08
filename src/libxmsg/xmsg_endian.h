/*
 * xmsg_endian.h - read and write big-endian words from byte buffers.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Every SINTRAN and XMSG field on the wire is big-endian (ND machines are big-endian 16-bit).
 * These are the only functions in the library that turn bytes into words and back; no struct is
 * ever laid over a frame, because that would depend on the host's byte order and on the compiler
 * honouring a packing pragma (house rule 12.3).
 */
#ifndef XMSG_ENDIAN_H
#define XMSG_ENDIAN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Read a big-endian 16-bit word.
 * @param p Points at the high byte; two bytes are read.
 * @return The word.
 */
static inline uint16_t xmsg_be16_read(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/**
 * @brief Write a big-endian 16-bit word.
 * @param p Receives two bytes, high byte first.
 * @param v The word.
 */
static inline void xmsg_be16_write(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

/**
 * @brief Read a big-endian 32-bit word.
 * @param p Points at the high byte; four bytes are read.
 * @return The word.
 */
static inline uint32_t xmsg_be32_read(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/**
 * @brief Write a big-endian 32-bit word.
 * @param p Receives four bytes, high byte first.
 * @param v The word.
 */
static inline void xmsg_be32_write(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

#ifdef __cplusplus
}
#endif

#endif /* XMSG_ENDIAN_H */
