/*
 * tad_codec.h - reading and writing the chain of TAD messages inside a terminal data frame.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * A TAD frame body is a 4-byte head (a 16-bit lead word, 0 on terminal data, then the length
 * of what follows, big-endian) and then a CHAIN of messages, each `opcode, count, count bytes`
 * (count at most 255). Opcodes are the TAD_OP_* constants generated from the registry.
 *
 * Writing: the asker side (SINTRAN's own CONNECT-TO) packs its messages with no fill at all.
 * MEASURED in every asker frame of RetroIO pcap-decode-sample.json, e.g. the session setup
 * `06 00 1B 00 1C 01 00 FF 00` and the terminal setup
 * `0C 01 08 0D 02 00 00 0F 01 1B 1F 03 4C 01 04`. The writer here does exactly that and nothing
 * more. A HOST pads its messages to word offsets: tad_writer_add_host() below.
 *
 * Reading: a lone 0x00 between messages is the host's alignment fill and is skipped. 0x00 is
 * not a TAD opcode (the registry has none), so this cannot swallow a real message. A message is
 * stepped over by its DECLARED count; when a count runs past the end the message is reported
 * with the bytes that are present and the walk ends.
 */
#ifndef XMSG_TAD_CODEC_H
#define XMSG_TAD_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libxmsg/gen/tad_wire.h"
#include "libxmsg/xmsg_errors.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** The longest data field one TAD message carries. */
#define TAD_MAX_DATA 255u

/** Size of the head in front of the chain. */
#define TAD_BODY_HEAD_SIZE 4u

/* Opcodes seen on the wire that the registry does not name. MEASURED: every captured asker
 * session setup and host port assignment in RetroIO pcap-decode-sample.json. */
#define TAD_OP_RAW_06 0x06u /**< session setup, first element, no data */
#define TAD_OP_RAW_07 0x07u /**< port assignment: 5 bytes, 00 + host system + session port */
#define TAD_OP_RAW_1B 0x1Bu /**< session setup, no data */
#define TAD_OP_RAW_1C 0x1Cu /**< session setup, one data byte 00 */
#define TAD_OP_RAW_FD 0xFDu /**< 7POLL: a notice from *TADADM; nothing is sent back */

/* 7WHO and 7IAM: the symbol names and values are in the SINTRAN J, K03, L07 and M06 symbol
 * tables (7WHO=000374, 7IAM=000050 octal). MEASURED on the wire in RetroIO
 * pcap-decode-sample.json: 7WHO in records 3653-3665 (node 103 to 102, bare control frame,
 * body FC 00), each answered by 7IAM in records 3618-3630 (102 back to the asking port). TADA
 * sends 7WHO to fill the TERMNO, USER and SYSTEM columns of a remote terminal and waits for
 * the answer; without one it hangs (live D100, 2026-11-09). */
#define TAD_OP_WHO        0xFCu /**< "who are you": no data */
#define TAD_OP_IAM        0x28u /**< "I am": no data, then the elements below */
#define TAD_OP_IAM_SYS    0x0Au /**< in 7IAM: 33 bytes, our system name + "'" + zeros */
#define TAD_OP_IAM_USER   0x10u /**< in 7IAM: 17 bytes, the user name + "'" + zeros */
#define TAD_IAM_SYS_SIZE  33u
#define TAD_IAM_USER_SIZE 17u
#define TAD_OP_RAW_FF     0xFFu /**< last element of the setup chains */

/** Low byte of the SYCN data 00 0B: the login has ended (after LOGOUT, with "--EXIT--").
 *  MEASURED: RetroIO pcap-decode-sample.json records 337, 617, 668, 856, 1337, 1373, 1408, 1515. */
#define TAD_SYCN_LOGGED_OUT 0x0Bu

/** A view of one message in a chain. */
typedef struct TadMessage
{
    uint8_t opcode;
    uint8_t count;       /**< declared data length */
    const uint8_t *data; /**< the data bytes present */
    uint8_t len;         /**< data bytes present (less than count only at a cut chain) */
} TadMessage;

/** Reader state over a chain. */
typedef struct TadReader
{
    const uint8_t *chain;
    size_t len;
    size_t pos;
} TadReader;

/**
 * @brief Start reading a chain (the bytes after the 4-byte head).
 * @param r     Reader.
 * @param chain The chain.
 * @param len   Its length.
 */
void tad_reader_init(TadReader *r, const uint8_t *chain, size_t len);

/**
 * @brief Read the next message, skipping fill bytes.
 * @param r   Reader.
 * @param out Receives the message.
 * @return true when a message was read; false at the end of the chain.
 */
bool tad_reader_next(TadReader *r, TadMessage *out);

/**
 * @brief Whether a chain contains a message with this opcode.
 * @param chain  The chain.
 * @param len    Its length.
 * @param opcode The opcode.
 * @return true when present.
 */
bool tad_chain_has(const uint8_t *chain, size_t len, uint8_t opcode);

/** Writer state over a caller's buffer. */
typedef struct TadWriter
{
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool overflow;
} TadWriter;

/**
 * @brief Start a packed chain in a caller's buffer.
 * @param w   Writer.
 * @param buf Buffer.
 * @param cap Its size.
 */
void tad_writer_init(TadWriter *w, uint8_t *buf, size_t cap);

/**
 * @brief Append one message.
 * @param w      Writer.
 * @param opcode TAD_OP_*.
 * @param data   Data bytes; may be NULL when len is 0.
 * @param len    0..255.
 * @return XMSG_OK; XMSG_EINVAL for len > 255; XMSG_ENOSPC when it does not fit (the writer is
 *         then marked overflowed and nothing more is appended).
 */
int tad_writer_add(TadWriter *w, uint8_t opcode, const uint8_t *data, size_t len);

/**
 * @brief Append one message the way a SINTRAN host lays it out, with a 0x00 fill byte first
 *        when the message would start on the wrong offset (offsets count from the chain start).
 *
 * The rule, from the version J NPL source as the C# TadMessageBuilder.cs quotes it
 * (20-COS-TAD-POF-CODE.NPL): CREMES lays a message header on an EVEN byte; CRHEOD lays it on an
 * ODD byte ("EVEN START, CREATE PAD BYTE") so that a payload of whole words lands on a word.
 * CRHEOD is used for ECKM and BMMX. The port assignment's 0x07 has the same shape (its builder,
 * TADADM, is not in the source). MEASURED against every host chain in
 * extras/tests/vectors/tad_connect_d100_d102.h: the banner `00 04 03 01 00 00 00 03 01 01 01 ..`,
 * the password prompt `01 0A <10> 00 03 01 FF 02 00`, `01 01 5E 00 02 00`, and the port
 * assignment `00 07 05 .. 1F 03 .. 00 0B 02 ..`. (The C# builder pads an ODD start twice
 * before ECKM/BMMX; CRHEOD, as quoted, pads only an even one. No capture has that case.)
 * @return As tad_writer_add().
 */
int tad_writer_add_host(TadWriter *w, uint8_t opcode, const uint8_t *data, size_t len);

/**
 * @brief Bytes written so far.
 * @param w Writer.
 * @return The length, or 0 after an overflow.
 */
size_t tad_writer_length(const TadWriter *w);

/**
 * @brief Give a 7-bit character even parity in bit 7, as the asker sends keystrokes.
 *        MEASURED: "sys" + CR goes out as F3 F9 F3 8D.
 * @param c The character; bit 7 is ignored.
 * @return The byte to send.
 */
uint8_t tad_even_parity(uint8_t c);

/**
 * @brief Write the 4-byte head in front of a chain placed at out + 4.
 * @param out       Body buffer.
 * @param lead      The lead word: 0 for terminal data.
 * @param chain_len The chain length.
 */
void tad_body_head_write(uint8_t *out, uint16_t lead, uint16_t chain_len);

/**
 * @brief Split a received body into head and chain.
 * @param body  The body (offset 28 of the frame onward).
 * @param len   Its length.
 * @param lead  Receives the lead word.
 * @param chain Receives a pointer to the chain.
 * @param clen  Receives the chain length as declared, clamped to what is present.
 * @return XMSG_OK, or XMSG_EINVAL for a NULL argument or fewer than 4 bytes.
 */
int tad_body_split(const uint8_t *body, size_t len, uint16_t *lead, const uint8_t **chain,
                   size_t *clen);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_TAD_CODEC_H */
