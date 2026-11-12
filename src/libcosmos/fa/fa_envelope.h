/*
 * fa_envelope.h - the 8-byte head of every COSMOS file-access (FA) message, and its counters.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 *   offset 0  type          u16  07F0 request/reply/data, 07A2 short ack, 0782 session finished,
 *                                07D2 connection confirm, 07C0 close
 *   offset 2  conversation  u16  the OTHER side's number: a client writes the server's, a server
 *                                writes the client's
 *   offset 4  counter       u8   see below
 *   offset 5  00
 *   offset 6  token         u16  client: 0001 on its first request, D761 after; ShortAck 8485;
 *                                server: 90BB (0001 on the second half of a data block),
 *                                ShortAck 922A
 *
 * The message counter: the low 7 bits count every request or content message a side sends
 * (wrapping at 128); bit 7 is SET on all of them EXCEPT the first of a two-message content pair.
 * So a server's reply then block reads 80 81 .. 84 05 86 87 08 89. Short acks count on their own,
 * starting at 1, with bit 7 clear.
 *
 * Source: capture-read.txt (DOC/captures/FA-READ-WRITE-2026-08-04), RetroIO Fa/FaMessageCounter.cs
 * and FaClientConversation.NextAckCounter.
 */
#ifndef FA_ENVELOPE_H
#define FA_ENVELOPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define FA_ENVELOPE_SIZE 8u

#define FA_TYPE_REQUEST          0x07F0u /**< request, reply and data message */
#define FA_TYPE_SHORT_ACK        0x07A2u /**< "received", one per request or content message */
#define FA_TYPE_SESSION_FINISHED 0x0782u /**< the client's last word */
#define FA_TYPE_CONFIRM          0x07D2u /**< the server's answer to the connect letter */
#define FA_TYPE_CLOSE            0x07C0u /**< the server's answer to session finished */

#define FA_TOKEN_CLIENT_FIRST 0x0001u /**< token on the client's first request */
#define FA_TOKEN_CLIENT       0xD761u /**< token on every later client request */
#define FA_TOKEN_CLIENT_ACK   0x8485u /**< token on a client short ack */
/** Token on the SECOND data message of a block the client writes; the first carries
 *  FA_TOKEN_CLIENT. MEASURED on all nine blocks of capture-write.txt (02:33:02.581 on). */
#define FA_TOKEN_CLIENT_DATA_SECOND 0x0001u

/** The 8-byte head. */
typedef struct FaEnvelope
{
    uint16_t type;
    uint16_t conversation;
    uint8_t counter;
    uint16_t token;
} FaEnvelope;

/**
 * @brief Read the head of a message.
 * @return true when `len` is at least FA_ENVELOPE_SIZE.
 */
bool fa_envelope_parse(const uint8_t *body, size_t len, FaEnvelope *e);

/** @brief Write the head into out[0..7]. */
void fa_envelope_write(uint8_t *out, const FaEnvelope *e);

/** The message counter (requests and content). */
typedef struct FaCounter
{
    uint8_t next; /**< the next 7-bit value */
} FaCounter;

/**
 * @brief Take the counter byte for the next request or content message.
 * @param c          The counter.
 * @param first_half true for the first message of a two-message content pair (bit 7 clear).
 * @return The byte to put at offset 4.
 */
uint8_t fa_counter_take(FaCounter *c, bool first_half);

#ifdef __cplusplus
}
#endif

#endif /* FA_ENVELOPE_H */
