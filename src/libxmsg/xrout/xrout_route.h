/*
 * xrout_route.h - XSGSY, "get routing info for system N": the routing table and its messages.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * XSGSY is XROUT service 75 (0x4B). The XMSG-COMMAND program asks it once per system when
 * LIST-ROUTING-INFO runs, of its own XROUT and of the remote XROUTs on the path, so a node that
 * does not answer leaves the operator's command waiting forever (measured 2026-11-09 on D100:
 * li-rout printed the L: and T: lines for system 101 and then hung, because node 101 only
 * acknowledged the request).
 *
 * The lookup rule (COSMOS Programmer Guide ND-60.164, XSGSY; C# InMemoryRoutingTable.cs
 * TryLookup): the answer is the FIRST entry whose system number is greater than or equal to the
 * one asked for; system 0 means "no more entries". Asking 1, then (answer + 1), and so on until 0
 * walks the whole table.
 *
 * Body of a request (C# ListRoutingClient.BuildRequest; every captured request):
 *
 *   01 4B 00 04   serial 01, service 0x4B XSGSY, 4 bytes of parameters
 *   01 02 hh ll   integer parameter 1 = the system asked for
 *
 * Body of a reply (C# ListRoutingServer.BuildResponse; every captured reply):
 *
 *   01 00 00 10   serial, status 0x00 XRSOK (bit 6 clear = a return message), 16 bytes
 *   01 02 hh ll   p1 system number (0 = no entry at or above the one asked)
 *   02 02 hh ll   p2 how it is reached, XROUT_REACH_* (an enum 0..4, not a bit field)
 *   03 02 hh ll   p3 detail: link index (neighbour), relay (via), subaddress, own number
 *   04 02 hh ll   p4 distance: low byte hops, high byte WANs crossed
 *
 * Values in real replies (NDInsight SINTRAN/XMSG/SRC/pcap-decode-report.txt):
 *   local      p2 4, p3 = the system itself, p4 0       (every machine about itself)
 *   neighbour  p2 1, p3 = link index 1 or 2, p4 1       (li-rout-103-tree: 100 about 102)
 *   via        p2 2, p3 = the relay system, p4 = hops   (li-route-d103-tree: 103 about 102)
 *   no entry   all four 0                               (li-route-d103-tree: asked 1111)
 */
#ifndef XMSG_XROUT_ROUTE_H
#define XMSG_XROUT_ROUTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libxmsg/xmsg_errors.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** How a system is reached: XSGSY parameter 2 (XMSG-PROTOCOL.md section 9.1). */
#define XROUT_REACH_UNAVAILABLE 0u
#define XROUT_REACH_NEIGHBOUR   1u /**< directly connected; detail = link index */
#define XROUT_REACH_VIA         2u /**< through another system; detail = that system */
#define XROUT_REACH_NETSERVER   3u /**< via a network server; detail = subaddress */
#define XROUT_REACH_LOCAL       4u /**< this machine; detail = its own system number */

/** The service byte of a request (XROUT_XSGSY, xrout_services.h). */
#define XROUT_ROUTE_SERVICE 0x4Bu

/** XMCSM of both request and reply: the high half of XMCSM_XSGSY_REQUEST (tad_wire.h). */
#define XROUT_ROUTE_XMCSM 0x0100u

/** Serial byte of every captured request and reply. What it means is not known. */
#define XROUT_ROUTE_SERIAL 0x01u

/** Sub-header bytes of every captured request (frame flags, role) and reply. */
#define XROUT_ROUTE_REQ_FRAME_FLAGS 0x86u
#define XROUT_ROUTE_REQ_ROLE        0x84u /**< XFWTF | XFROU; D100 also sends 0xC4 */
#define XROUT_ROUTE_REP_FRAME_FLAGS 0x86u
#define XROUT_ROUTE_REP_ROLE        0x60u

/** Body sizes. */
#define XROUT_ROUTE_REQ_SIZE 8u
#define XROUT_ROUTE_REP_SIZE 20u

/** Entries one table holds. */
#ifndef XROUT_ROUTE_MAX
#define XROUT_ROUTE_MAX 8
#endif

/** One routing-table entry: the four reply parameters. */
typedef struct XroutRouteEntry
{
    uint16_t system;   /**< p1; 0 = no entry */
    uint16_t how;      /**< p2; XROUT_REACH_* */
    uint16_t detail;   /**< p3; meaning depends on `how` */
    uint16_t distance; /**< p4; low byte hops, high byte WANs */
} XroutRouteEntry;

/** A small routing table, kept in ascending system order. */
typedef struct XroutRouteTable
{
    XroutRouteEntry e[XROUT_ROUTE_MAX];
    uint8_t count;
} XroutRouteTable;

/** @brief Hops: the low byte of the distance word. */
static inline uint8_t xrout_route_hops(const XroutRouteEntry *e)
{
    return (uint8_t)(e->distance & 0xFFu);
}

/** @brief WANs crossed: the high byte of the distance word. */
static inline uint8_t xrout_route_wans(const XroutRouteEntry *e)
{
    return (uint8_t)(e->distance >> 8u);
}

/** @brief Plain-words name of a XROUT_REACH_* value ("local", "neighbour", ...). */
const char *xrout_route_how_name(uint16_t how);

/** @brief Empty the table. */
void xrout_route_init(XroutRouteTable *t);

/**
 * @brief Add an entry, or replace the one with the same system number.
 * @return XMSG_OK; XMSG_EINVAL for system 0 (it means "no entry" on the wire); XMSG_ENOSPC
 *         when the table is full.
 */
int xrout_route_put(XroutRouteTable *t, const XroutRouteEntry *e);

/** @brief Whether the table has an entry for exactly this system. */
bool xrout_route_has(const XroutRouteTable *t, uint16_t system);

/**
 * @brief The XSGSY lookup: the first entry with system >= `asked`.
 * @param out Receives it, or all zeros (system 0) when there is none.
 * @return true when an entry was found.
 */
bool xrout_route_lookup(const XroutRouteTable *t, uint16_t asked, XroutRouteEntry *out);

/**
 * @brief Write a request body asking for `asked`.
 * @param out Receives XROUT_ROUTE_REQ_SIZE bytes.
 */
void xrout_route_request_write(uint8_t *out, uint16_t asked);

/**
 * @brief Read a request body.
 * @param body   Body from the XROUT head on.
 * @param len    Its length.
 * @param serial Receives the serial byte (NULL allowed).
 * @param asked  Receives parameter 1.
 * @return XMSG_OK; XMSG_EINVAL on a short body; XMSG_EPROTO when the service is not XSGSY or
 *         parameter 1 is missing.
 */
int xrout_route_request_read(const uint8_t *body, size_t len, uint8_t *serial, uint16_t *asked);

/**
 * @brief Write a reply body for an entry (all zeros = no entry).
 * @param out    Receives XROUT_ROUTE_REP_SIZE bytes.
 * @param serial The request's serial byte.
 * @param e      The entry.
 */
void xrout_route_reply_write(uint8_t *out, uint8_t serial, const XroutRouteEntry *e);

/**
 * @brief Read a reply body.
 * @param body   Body from the XROUT head on.
 * @param len    Its length.
 * @param status Receives the status byte (0 = XRSOK; NULL allowed).
 * @param e      Receives the four parameters; a missing one reads 0 (as the C# ParseResponse).
 * @return XMSG_OK; XMSG_EINVAL on a short body; XMSG_EPROTO when the status is not 0 (`status`
 *         is still set) or the parameters do not parse.
 */
int xrout_route_reply_read(const uint8_t *body, size_t len, uint8_t *status, XroutRouteEntry *e);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_XROUT_ROUTE_H */
