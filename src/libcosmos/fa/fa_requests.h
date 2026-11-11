/*
 * fa_requests.h - building the messages a COSMOS file-access CLIENT sends.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Every builder writes a whole message body (envelope included) and pads it to an EVEN length
 * with a zero byte: an odd body is dropped by the ND without a word. The pad byte a real client
 * sends is whatever was in its buffer (0A, 55, 8C, 0E in capture-read.txt); the reader ignores
 * it, so zero is as good.
 *
 * A request is
 *   07F0 <server conversation> <counter> 00 <token> | 92 <op> | 92 <sequence> | fields | F2 00FF
 * where the sequence is the step number counting from 1. The bytes of every builder are pinned
 * to capture-read.txt by extras/tests/test_cosmos_fa.c.
 */
#ifndef FA_REQUESTS_H
#define FA_REQUESTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libcosmos/fa/fa_envelope.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Operation numbers (the first 92 value of a request). Source: fa-qform.json. */
#define FA_OP_RESERVE        0x0002u
#define FA_OP_RELEASE        0x0003u
#define FA_OP_OPEN           0x0005u
#define FA_OP_CLOSE          0x0006u
#define FA_OP_SET_BLOCK_SIZE 0x0007u
#define FA_OP_READ           0x0008u
#define FA_OP_WRITE          0x0009u
#define FA_OP_SPECIAL        0x000Cu

/** Sub-function of FA_OP_SPECIAL that asks for the file's 64-byte object entry. */
#define FA_SPECIAL_FILE_INFO 0x0021u

/** Sub-functions of FA_OP_SPECIAL used by a directory walk (LIST-FILES, FILE-STATISTICS). */
#define FA_SPECIAL_NEXT_ENTRY 0x0078u /**< the next object entry matching the name block */
#define FA_SPECIAL_DIRECTORY  0x00A4u /**< the directory entry (its name, "PACK-ONE") */
#define FA_SPECIAL_USER       0x008Cu /**< the user entry (its name, "SYSTEM") */

/** The name block a walk carries, and the cursor of the first walk request. */
#define FA_WALK_BLOCK_SIZE   62u
#define FA_WALK_FIRST_CURSOR 0xFFFFu

/** Bytes in one FA block (one SINTRAN page): two data messages of 1024. */
#define FA_BLOCK_SIZE 2048u

/** Bytes of file content in one data message. */
#define FA_DATA_BYTES 1024u

/** A data message: the envelope and 1024 raw bytes, no QFORM. */
#define FA_DATA_MESSAGE_SIZE (FA_ENVELOPE_SIZE + FA_DATA_BYTES)

/** Largest body any builder here writes (the reserve is 112). */
#define FA_REQUEST_MAX 128u

/** Bit set on the sequence of every second block request, starting with the second. */
#define FA_SEQ_ALTERNATE_BIT 0x8000u

/**
 * @brief The connect letter: XROUT XSLET to port 0 of the server machine, asking for *FA-SERVER.
 *
 *   1B 41 <len> FF 0A "*FA-SERVER" FE <n> "<system>" | 07E2 0000 <word> 6400 A200 FF 00
 *
 * The length word counts the two string parameters only. The server echoes `word` as the
 * conversation number on everything it sends back. MEASURED: serial 1B and the trailer in every
 * captured client letter (capture-read.txt 02:29:10.703, word 0002; 02:29:40.923, word 0004).
 * @param out    Buffer.
 * @param cap    Its size.
 * @param system The server's system name, "D102".
 * @param word   Our conversation number.
 * @return Bytes written, or 0 when it does not fit.
 */
size_t fa_build_connect_letter(uint8_t *out, size_t cap, const char *system, uint16_t word);

/**
 * @brief Reserve (op 02): who asks, and into whose directory.
 *
 *   F2 0001 A2 07D0 | F2 0002 8C 06 92 0001 92 0001 | F2 0003 Bx "<asker>  <local user>" |
 *   F2 0004 8C 38 B0 10 "<remote user>'" 00.. E1 80 B0 10 <password word> 00.. B0 10 00.. | F2 00FF
 *
 * @param out         Buffer of at least FA_REQUEST_MAX.
 * @param cap         Its size.
 * @param env         Envelope (type, conversation, counter, token).
 * @param seq         Sequence number.
 * @param asker       The asking process, five characters as SINTRAN names it ("BAK04").
 * @param local_user  The user making the request.
 * @param remote_user The user whose directory is opened on the server.
 * @param password    That user's folded password word (sintran_password_fold), 0 for none.
 * @return Bytes written (even), or 0 when a name does not fit its field.
 */
size_t fa_build_reserve(uint8_t *out, size_t cap, const FaEnvelope *env, uint16_t seq,
                        const char *asker, const char *local_user, const char *remote_user,
                        uint16_t password);

/**
 * @brief Open for reading (op 05): F2 0002 <string "<spec>'."> F2 00FF.
 *
 * The character after the apostrophe: RetroIO's live-proven reader sends "."; the D100 client
 * in the captures sent ".", "T" and "U" (BUGS.md C1: not understood). "." is sent.
 * @param spec The file as the server names it: "(USER)NAME:TYPE", or "NAME:TYPE".
 * @return Bytes written, or 0 when the specification is longer than 253 characters.
 */
size_t fa_build_open_read(uint8_t *out, size_t cap, const FaEnvelope *env, uint16_t seq,
                          const char *spec);

/** @brief Set block size (op 07): F2 0001 A2 <size> F2 00FF. */
size_t fa_build_set_block_size(uint8_t *out, size_t cap, const FaEnvelope *env, uint16_t seq,
                               uint16_t size);

/** @brief File information (op 0C): F2 0001 92 0021 F2 00FF. */
size_t fa_build_file_info(uint8_t *out, size_t cap, const FaEnvelope *env, uint16_t seq);

/** @brief Read a block (op 08): F2 0001 A4 <block> F2 00FF. */
size_t fa_build_read_block(uint8_t *out, size_t cap, const FaEnvelope *env, uint16_t seq,
                           uint32_t block);

/**
 * @brief Build the 62-byte name block of a walk: "(<user>)<spec>'<spec>'", zero filled.
 *
 * MEASURED: FILE-STATISTICS of SINTRAN:DATA sent "(SYSTEM)SINTRAN:DATA'SINTRAN:DATA'" and zeros
 * (file-statistics capture, 2026-07-29 20:15). A LIST-FILES with no name sent "(SYSTEM)'" and
 * then bytes left over in the client's buffer; here the empty name is written "(<user>)''"
 * and zeros, the same rule with a name of length 0 (unverified that a server reads past the
 * first apostrophe).
 * @param block Receives FA_WALK_BLOCK_SIZE bytes.
 * @param user  The directory owner, "SYSTEM".
 * @param spec  The name, "SINTRAN:DATA", or "" for every file.
 * @return true when it fits.
 */
bool fa_build_walk_block(uint8_t *block, const char *user, const char *spec);

/**
 * @brief Next entry (op 0C/78): F2 0001 92 0078 F2 0002 8C 80 46 B0 3E <block> A2 <user index>
 *        A2 <cursor> F2 00FF.
 *
 * The first request sends user index 0 and cursor FFFF; each later one sends the user index
 * and the object index of the last entry received, plus one. MEASURED in capture-list-files.txt
 * (user SYSTEM, index 0) and the SECRET listing capture (2026-07-29 14:26: index 7). The
 * constructed length is written in the 0x80 marker form, as the real client does.
 */
size_t fa_build_walk_next(uint8_t *out, size_t cap, const FaEnvelope *env, uint16_t seq,
                          const uint8_t *block, uint16_t user_index, uint16_t cursor);

/**
 * @brief Directory or user entry (op 0C/A4 or 0C/8C): F2 0001 92 <sub> F2 0002 8C 80 03
 *        A2 <index> F2 00FF.
 *
 * MEASURED: the directory question sent index 0 in both captured listings; the user question
 * sent the user index from the first entry (0 for SYSTEM, 7 for SECRET).
 */
size_t fa_build_special_index(uint8_t *out, size_t cap, const FaEnvelope *env, uint16_t seq,
                              uint16_t sub, uint16_t index);

/** @brief A request with no fields (close 06, release 03): 92 op 92 seq F2 00FF. */
size_t fa_build_bare(uint8_t *out, size_t cap, const FaEnvelope *env, uint16_t op, uint16_t seq);

/** @brief Short ack: 07A2 <server conversation> <counter> 00 8485 (8 bytes). */
size_t fa_build_short_ack(uint8_t *out, size_t cap, uint16_t conversation, uint8_t counter);

/**
 * @brief Session finished: 0782 <server conversation> <our word> 8000 0000 (10 bytes).
 * MEASURED: capture-read.txt 02:29:16.631; the server answers 07C0 and frees its seat.
 */
size_t fa_build_session_finished(uint8_t *out, size_t cap, uint16_t conversation,
                                 uint16_t word);

#ifdef __cplusplus
}
#endif

#endif /* FA_REQUESTS_H */
