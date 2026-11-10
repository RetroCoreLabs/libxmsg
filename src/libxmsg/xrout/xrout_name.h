/*
 * xrout_name.h - XSGNI, "get next name": the name table and its messages.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * XSGNI is XROUT service 69 (0x45). LIST-NAMES (XMSG-COMMAND) asks it of a remote XROUT (port 0)
 * again and again to read that machine's whole name table: the system names (aliases) and the
 * named ports (*TADADM, *FA-FSA, ...). A node that only acknowledges the request leaves the
 * operator's command waiting forever (seen on D100: LIST-NAMES with "XROUT system? 101" got an
 * ACK from libxmsg and then nothing).
 *
 * Every byte below is from one complete walk, captured 2026-11-10 on the COSMOS Ethernet hub
 * (TCP 5010): D102 ran LIST-NAMES against "XROUT system? d100", 14 requests, 14 replies.
 *
 * A row is keyed by its MAGIC: system16:portword16 as one 32-bit number. A system name (alias)
 * is the row with port word 0x0001; a named port has the port's own word, and LIST-NAMES prints
 * it as portword >> 7 (0x01DA -> 3, 0x0401 -> 8). The rows come back in ascending magic order.
 *
 * Body of a request (sub-header 86 84, D100 also sends 86 C4; XMCSM 0x0100):
 *
 *   01 45 00 04         serial 01, service 0x45 XSGNI, 4 bytes of parameters
 *   01 02 00 00         the FIRST call: integer parameter 1 = 16-bit 0
 * or
 *   01 45 00 06         6 bytes of parameters
 *   01 04 mm mm mm mm   every NEXT call: parameter 1 = 32-bit magic = the previous reply's + 1
 *
 * Body of a reply (sub-header 86 60, both address fields = the asker's system and port):
 *
 *   01 00 ll ll         serial echoed, status 00, parameter bytes
 *   01 04 mm mm mm mm   parameter 1: the row's magic (first row with magic >= the one asked)
 *   FE nn <name>        string parameter 2 (type byte -2): the name; when nn is odd a 00 pad
 *                       byte follows and is counted in ll, even when nothing comes after it
 *                       (*TADADM: ... FE 07 "*TADADM" 00, ll = 0x10)
 *   03 02 ff ff         integer parameter 3: free seats - present ONLY for connection ports
 *                       (*XFTRA 1, *FA-FSA 2, *FA-FSA-I 0; absent for D100, *TADADM, *COSPO)
 *
 * End of table: 01 00 00 04 01 02 00 00, parameter 1 = 16-bit 0 (magic 0, no name).
 */
#ifndef XMSG_XROUT_NAME_H
#define XMSG_XROUT_NAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libxmsg/xmsg_errors.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** The service byte of a request (XROUT_XSGNI = 69, xrout_services.h). */
#define XROUT_NAME_SERVICE 0x45u

/** XMCSM of request and reply: 0x0100 on every captured XSGNI frame (as on XSGSY). */
#define XROUT_NAME_XMCSM 0x0100u

/** Serial our requests carry: 01, as in the capture. The answer echoes whatever came. */
#define XROUT_NAME_SERIAL 0x01u

/** Sub-header bytes of the captured requests and replies. */
#define XROUT_NAME_REQ_FRAME_FLAGS 0x86u
#define XROUT_NAME_REQ_ROLE        0x84u /**< 102's requests; D100's own request had 0xC4 */
#define XROUT_NAME_REP_FRAME_FLAGS 0x86u
#define XROUT_NAME_REP_ROLE        0x60u

/** Port word of a system-name (alias) row. */
#define XROUT_NAME_SYSTEM_PORT 0x0001u

/** "No free-seat count" in a row: the reply has no parameter 3. */
#define XROUT_NAME_NO_FREE (-1)

/** Longest name a row holds. The longest captured is 9 ("*XM-ENNS0", "*FA-FSA-I"); the real
 *  XROUT limit is not known (unverified), so a longer name is refused, never cut. */
#ifndef XROUT_NAME_LEN_MAX
#define XROUT_NAME_LEN_MAX 16
#endif

/** Rows one table holds (the captured D100 table has 13). */
#ifndef XROUT_NAME_ROWS_MAX
#define XROUT_NAME_ROWS_MAX 24
#endif

/** Largest request body (the 32-bit form) and reply body (longest name, pad, free count). */
#define XROUT_NAME_REQ_MAX 10u
#define XROUT_NAME_REP_MAX (4u + 6u + 2u + XROUT_NAME_LEN_MAX + 1u + 4u)

/** One row of a name table. */
typedef struct XroutNameRow
{
    uint16_t system;                    /**< high half of the magic */
    uint16_t port;                      /**< low half; XROUT_NAME_SYSTEM_PORT for a system name */
    int32_t free;                       /**< free seats, or XROUT_NAME_NO_FREE */
    char name[XROUT_NAME_LEN_MAX + 1u]; /**< NUL ended */
} XroutNameRow;

/** A name table, kept in ascending magic order. */
typedef struct XroutNameTable
{
    XroutNameRow r[XROUT_NAME_ROWS_MAX];
    uint8_t count;
} XroutNameTable;

/** @brief The row's magic: system16:portword16. */
static inline uint32_t xrout_name_magic(const XroutNameRow *r)
{
    return ((uint32_t)r->system << 16u) | r->port;
}

/** @brief Whether the row is a system name (port word 0x0001). */
static inline bool xrout_name_is_system(const XroutNameRow *r)
{
    return r->port == XROUT_NAME_SYSTEM_PORT;
}

/** @brief Empty the table. */
void xrout_name_init(XroutNameTable *t);

/**
 * @brief Fill a row.
 * @param r      Receives it.
 * @param system System number (not 0).
 * @param port   Port word (not 0; XROUT_NAME_SYSTEM_PORT for a system name).
 * @param name   1..XROUT_NAME_LEN_MAX characters.
 * @param free   Free seats 0..65535, or XROUT_NAME_NO_FREE.
 * @return XMSG_OK; XMSG_EINVAL for NULL, system or port 0 (magic 0 means "end" on the wire), an
 *         empty or too long name, or a free count out of range.
 */
int xrout_name_row_set(XroutNameRow *r, uint16_t system, uint16_t port, const char *name,
                       int32_t free);

/**
 * @brief Add a row, or replace the one with the same magic.
 * @return XMSG_OK; XMSG_EINVAL for a row xrout_name_row_set() would refuse; XMSG_ENOSPC when the
 *         table is full.
 */
int xrout_name_put(XroutNameTable *t, const XroutNameRow *r);

/**
 * @brief Remove the row with this system and port word.
 * @return XMSG_OK, or XMSG_EINVAL when there is none.
 */
int xrout_name_remove(XroutNameTable *t, uint16_t system, uint16_t port);

/** @brief Whether the table has a row with exactly this system and port word. */
bool xrout_name_has(const XroutNameTable *t, uint16_t system, uint16_t port);

/**
 * @brief The XSGNI lookup: the first row with magic >= `asked`.
 * @param t     The table (NULL reads as empty).
 * @param asked The magic asked for.
 * @param out   Receives the row (all zeros, free XROUT_NAME_NO_FREE, when there is none).
 * @return true when a row was found.
 */
bool xrout_name_lookup(const XroutNameTable *t, uint32_t asked, XroutNameRow *out);

/**
 * @brief Write a request body.
 * @param out   Receives XROUT_NAME_REQ_MAX bytes at most.
 * @param asked 0 for the first call (written as the captured 16-bit 0), else the magic to start
 *              at (the previous reply's + 1, written as 32 bits).
 * @return Bytes written: 8 or 10.
 */
size_t xrout_name_request_write(uint8_t *out, uint32_t asked);

/**
 * @brief Read a request body.
 * @param body   Body from the XROUT head on.
 * @param len    Its length.
 * @param serial Receives the serial byte (NULL allowed).
 * @param asked  Receives parameter 1 (a 1-, 2- or 4-byte integer).
 * @return XMSG_OK; XMSG_EINVAL on NULL or a short body; XMSG_EPROTO when the service is not
 *         XSGNI or parameter 1 is missing.
 */
int xrout_name_request_read(const uint8_t *body, size_t len, uint8_t *serial, uint32_t *asked);

/**
 * @brief Write a reply body: a row, or the end form when `r` is NULL.
 * @param out    Receives XROUT_NAME_REP_MAX bytes at most.
 * @param serial The request's serial byte.
 * @param r      The row, or NULL for "no more".
 * @return Bytes written, or 0 when the row's name is empty or longer than XROUT_NAME_LEN_MAX.
 */
size_t xrout_name_reply_write(uint8_t *out, uint8_t serial, const XroutNameRow *r);

/**
 * @brief Read a reply body.
 * @param body   Body from the XROUT head on.
 * @param len    Its length.
 * @param status Receives the status byte (0 = XRSOK; NULL allowed).
 * @param r      Receives the row; on the end form all zeros with free XROUT_NAME_NO_FREE.
 * @param is_end Receives true for the end form (magic 0).
 * @return XMSG_OK; XMSG_EINVAL on NULL or a short body; XMSG_EPROTO when the status is not 0
 *         (`status` is still set), parameter 1 is missing, a row has no name, or the name is
 *         longer than XROUT_NAME_LEN_MAX (refused, never cut).
 */
int xrout_name_reply_read(const uint8_t *body, size_t len, uint8_t *status, XroutNameRow *r,
                          bool *is_end);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_XROUT_NAME_H */
