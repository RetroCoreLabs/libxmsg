/*
 * xmsg_flags1.h - the outgoing datagram number per peer machine, and the rules that keep it in
 * step with what the peer expects.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * THE FLAGS1 LAW (registry sintran-wire.json, MEASURED): Flags1 is a message number belonging to a
 * (sender, peer) pair, one counter per peer we talk to, shared by every session on that peer,
 * zeroed only at bring-up and never in use. The peer keeps its own expectation of us and never
 * sends it. A number BEHIND that expectation is dropped in silence; a number AHEAD is refused
 * with an error frame carrying XENSE (-34). So, as the C# XmsgServerHost records after a day of
 * losing frames to it:
 *
 *  - Allocate at send and PERSIST AT SEND (the kernel does: the send path reads XNSEQ, stamps it,
 *    increments and stores it back; nothing waits for an acknowledgement). A store that advanced
 *    on the peer's ACK ended runs with the saved value below the peer's count, and everything
 *    after a restart was dropped.
 *  - Never couple our count to the peer's numbers. Echoing a received Flags1 (an ACK) spends
 *    none of ours. A ratchet that pulled our count up to the echoed value plus one jumped it
 *    eight ahead on a hub and drew 127 XENSE rejections and a torn-down link (2026-08-10).
 *  - A received reachability request (0x19) is the peer's XMSG restart: its expectation of us is
 *    zero again, so our count to it is reset to zero. Sending our OWN 0x19 does not touch ours.
 *  - On XENSE for a frame we originated, step the count DOWN to one below the refused number and
 *    let the session retry; give up after a few steps rather than storm.
 *
 * Persistence goes through the application's port with a saving policy for flash: the stored
 * value is an UPPER BOUND on the next number we will use, re-saved every `persist_every` frames.
 * After a restart we resume at the bound, so we are at worst a few ahead (recoverable by XENSE
 * step-down), never behind (silent loss). With persist_every = 1 the bound is exactly the next
 * number, as the C# saves it.
 */
#ifndef XMSG_FLAGS1_H
#define XMSG_FLAGS1_H

#include <stdbool.h>
#include <stdint.h>

#include "libxmsg/xmsg_config.h"
#include "libxmsg/xmsg_errors.h"
#include "libxmsg/xmsg_ports.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** XENSE step-downs tolerated for one peer before giving up (C# MaxOriginationResyncAttempts). */
#define XMSG_FLAGS1_MAX_XENSE_STEPS 8

/** One peer's counter. */
typedef struct XmsgFlags1Entry
{
    bool in_use;
    uint16_t node;        /**< the peer */
    uint16_t next;        /**< the number the next originated frame takes */
    uint16_t saved_bound; /**< the value last written to the store */
    uint8_t xense_steps;  /**< consecutive XENSE step-downs since the last delivery */
} XmsgFlags1Entry;

/** The table. */
typedef struct XmsgFlags1Table
{
    XmsgFlags1Entry entries[XMSG_MAX_PEERS];
    XmsgFlags1Port port;
    uint8_t persist_every; /**< save the bound every this many allocations; at least 1 */
    bool store_missing_warned;
} XmsgFlags1Table;

/**
 * @brief Set up the table.
 * @param t             The table.
 * @param port          The store; may be NULL or have NULL functions (counters then start at 0).
 * @param persist_every 1 saves every frame; larger values spare flash at the cost of being up to
 *                      that many ahead after a restart.
 */
void xmsg_flags1_init(XmsgFlags1Table *t, const XmsgFlags1Port *port, uint8_t persist_every);

/**
 * @brief Take the next number for a frame to `node` and advance the counter.
 * @param t    The table.
 * @param node The peer.
 * @param out  Receives the number. 0xFFFF is never handed out (it means "reachability").
 * @return XMSG_OK, or XMSG_ENOSPC when XMSG_MAX_PEERS peers are already tracked.
 */
int xmsg_flags1_allocate(XmsgFlags1Table *t, uint16_t node, uint16_t *out);

/**
 * @brief The number the next allocation would return, without taking it.
 * @param t    The table.
 * @param node The peer.
 * @return The number, or 0 for an unknown peer with nothing stored.
 */
uint16_t xmsg_flags1_peek(XmsgFlags1Table *t, uint16_t node);

/**
 * @brief The peer restarted (its reachability request arrived): our count to it is zero again.
 * @param t    The table.
 * @param node The peer.
 */
void xmsg_flags1_reset(XmsgFlags1Table *t, uint16_t node);

/**
 * @brief The peer refused one of our frames with XENSE: step the count down.
 * @param t       The table.
 * @param node    The peer.
 * @param refused The Flags1 it refused.
 * @return true when the count was stepped to `refused - 1`; false when giving up (too many steps
 *         in a row, or the refused number was 0) or when `refused` was not sent at the current
 *         count (it is not below `next`: a frame from before a reset or an earlier step), in
 *         which case the count is left alone.
 */
bool xmsg_flags1_on_xense(XmsgFlags1Table *t, uint16_t node, uint16_t refused);

/**
 * @brief The peer acknowledged one of our frames: the sequence is accepted, forget any steps.
 * @param t    The table.
 * @param node The peer.
 */
void xmsg_flags1_on_delivered(XmsgFlags1Table *t, uint16_t node);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_FLAGS1_H */
