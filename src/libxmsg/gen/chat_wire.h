/* Generated from DOC/protocols/chat-wire.json by generate_c.py. DO NOT EDIT. */
#ifndef XMSG_CHAT_WIRE_H
#define XMSG_CHAT_WIRE_H

#include <stdint.h>

/*
 * the one protocol Norsk Data never shipped: joining a room, saying something, and the two
 * ports that decide who spends a seat
 */

/*
 * chat_message_prefix - 4 bytes, big-endian on the wire. Accessors rather than a packed
 * struct: a packed struct depends on the host's byte order and on the compiler honouring a
 * pragma, while these read the bytes as defined.
 */
#define CHAT_MESSAGE_PREFIX_SIZE 4

/*
 * which message this is - see message_kinds -- MEASURED: ChatMessageGoldenBytesTests pins the
 * encoding both directions
 */
static inline uint8_t chat_message_prefix_read_kind(const uint8_t *frame)
{
    return frame[0];
}

/*
 * how many ASCII bytes of nickname follow. Zero is legal and means the message carries no
 * name. -- MEASURED: ChatMessageGoldenBytesTests; a /who request carries kind 11 with an empty
 * name field
 */
static inline uint8_t chat_message_prefix_read_nickname_length(const uint8_t *frame)
{
    return frame[1];
}

#endif /* XMSG_CHAT_WIRE_H */
