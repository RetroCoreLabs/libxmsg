/*
 * xrout_letter.h - the letter that reaches a named service on another machine, and its answer.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * XROUT is SINTRAN's name service. A task that knows a service only by name ("*TADADM",
 * "*FA-SERVER") sends an XSLET letter (service 65 = 0x41) to port 0 of the far machine; XROUT
 * looks the name up, spends one of the service's free seats, and forwards the letter to the port
 * that holds the name. From then on the two sides talk directly.
 *
 * Body layout of every XROUT message and of every data frame TAD sends (C# XroutMessage.cs,
 * XmsgDataFields.ComposeBody): a 4-byte head, then the payload.
 *
 *   byte 0   serial       (0 on everything we send)
 *   byte 1   service      0x41 XSLET on a letter; on a reply, unchanged when accepted or
 *                         OVERWRITTEN with the XROUT error code when refused (XRUNN 2 = no such
 *                         name, XRNSP 4 = no free seat, ...)
 *   bytes 2-3 length      of the payload that follows, big-endian
 *   payload  parameters: a type byte (positive n = integer parameter n, negative = string
 *            parameter n), a length byte, the bytes; a 0x00 fill byte before a parameter that
 *            would start on an odd offset. The final parameter is not padded.
 *
 * Captured TAD connect letter: 00 41 00 10 FF 07 "*TADADM" 00 FE 04 "D102" - string parameter 1
 * is the service name, string parameter 2 the target system. Captured accept: 00 41 00 08 01 02
 * 00 00 02 02 00 0A - integer parameters 1 and 2 (sessions in use and free, as the C# reads them).
 */
#ifndef XMSG_XROUT_LETTER_H
#define XMSG_XROUT_LETTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libxmsg/xmsg_errors.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define XROUT_HEAD_SIZE     4u
#define XROUT_SERVICE_XSLET 0x41u
#define XROUT_MAX_PARAMS    8

/** One decoded parameter. */
typedef struct XroutParam
{
    uint8_t number;      /**< 1..127 */
    bool is_string;      /**< string (negative type byte) or integer */
    const uint8_t *data; /**< the bytes, in the caller's buffer */
    uint8_t len;
} XroutParam;

/** A decoded XROUT message. */
typedef struct XroutMessage
{
    uint8_t serial;
    uint8_t service; /**< 0x41 on an accepted reply; an XR* error code on a refusal */
    uint16_t length; /**< declared payload length */
    XroutParam params[XROUT_MAX_PARAMS];
    uint8_t param_count;
} XroutMessage;

/**
 * @brief Write the 4-byte head: serial, service, payload length.
 */
void xrout_head_write(uint8_t *out, uint8_t serial, uint8_t service, uint16_t payload_len);

/**
 * @brief Build an XSLET letter to a named service on a named system.
 * @param out      Output buffer.
 * @param cap      Its size; 4 + 2 + strlen(service) + fill + 2 + strlen(system) bytes needed.
 * @param service  The service name as registered, e.g. "*TADADM" (at most 255 bytes).
 * @param system   The target system name as the far XROUT knows it, e.g. "D102".
 * @return Bytes written (head included), or 0 when it does not fit.
 */
size_t xrout_letter_build(uint8_t *out, size_t cap, const char *service, const char *system);

/**
 * @brief Parse an XROUT message (a reply to a letter, or a letter arriving at our port 0).
 * @param body Bytes from the head onward.
 * @param len  Count.
 * @param m    Receives the fields; parameter data points into `body`.
 * @return XMSG_OK; XMSG_EINVAL when shorter than the head; XMSG_EPROTO when a parameter runs
 *         past the end (what was parsed before it is kept).
 */
int xrout_parse(const uint8_t *body, size_t len, XroutMessage *m);

/**
 * @brief Whether a parsed reply accepted the letter (service byte still XSLET).
 */
static inline bool xrout_reply_accepted(const XroutMessage *m)
{
    return m->service == XROUT_SERVICE_XSLET;
}

/** @brief The integer value of parameter `number`, or -1 when absent or not an integer. */
int32_t xrout_param_int(const XroutMessage *m, uint8_t number);

/** @brief Plain-words name of an XROUT error code (the service byte of a refusal). */
const char *xrout_error_name(uint8_t code);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_XROUT_LETTER_H */
