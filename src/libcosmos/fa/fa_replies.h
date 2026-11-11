/*
 * fa_replies.h - reading what a COSMOS file-access server sends back.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * A reply has the request's shape: 07F0 <our word> <counter> 00 90BB | 92 <op> | 92 <seq> |
 * fields | F2 00FF. A refusal carries F2 0001 A2 <SINTRAN error> (46 no such file, 48 wrong
 * password, 197 end of directory); a success has no selector 1. A data message is 1032 bytes:
 * the envelope and 1024 raw bytes, no QFORM.
 *
 * Source: capture-read.txt, capture-open-error.txt; RetroIO Fa/FaRefusalCodec.cs,
 * FaOpenFileCodec.cs.
 */
#ifndef FA_REPLIES_H
#define FA_REPLIES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libcosmos/fa/fa_envelope.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** SINTRAN error numbers seen in refusals. */
#define FA_ERR_NO_SUCH_FILE     46u
#define FA_ERR_WRONG_PASSWORD   48u
#define FA_ERR_END_OF_DIRECTORY 197u

/** A parsed reply. */
typedef struct FaReply
{
    FaEnvelope env;
    uint16_t op;           /**< the operation it answers */
    uint16_t seq;          /**< the sequence of the request it answers */
    const uint8_t *fields; /**< the QFORM after op and seq */
    size_t fields_len;
    bool refused;          /**< selector 1 carries a SINTRAN error */
    uint16_t error;        /**< that error, when refused */
} FaReply;

/**
 * @brief Parse a reply body (not a data message).
 * @return true when it has an envelope and the 92 op 92 seq head.
 */
bool fa_reply_parse(const uint8_t *body, size_t len, FaReply *r);

/**
 * @brief Read an open reply: selector 2 the file number (A2), selector 3 the length in bytes (A4).
 * @return true when both are there.
 */
bool fa_reply_open_info(const FaReply *r, uint16_t *file_number, uint32_t *length);

/**
 * @brief Find the 64-byte object entry in a file information reply: F2 0002 8C 42 B0 40 <64>.
 * @return A pointer to the 64 bytes, or NULL.
 */
const uint8_t *fa_reply_object_entry(const FaReply *r);

/** @brief Plain-words name of a SINTRAN error number from a refusal ("" when not known). */
const char *fa_error_name(uint16_t error);

#ifdef __cplusplus
}
#endif

#endif /* FA_REPLIES_H */
