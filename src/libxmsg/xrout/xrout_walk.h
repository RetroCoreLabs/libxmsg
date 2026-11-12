/*
 * xrout_walk.h - read a host's routing table: XSGSY asked again and again, the client side.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * This is what LIST-ROUTING-INFO does on a SINTRAN machine, pointed at the host we are linked
 * to: ask its XROUT (port 0) "first entry at or above system 1", then "at or above (answer + 1)",
 * and so on until the answer is system 0 (no more). Every entry found is handed to on_entry; at
 * the end on_done says how it went. Nothing blocks: the walk is a session on the node and moves
 * on node ticks and received frames.
 *
 * Wire form, from the captures and the C# ListRoutingClient.BuildRequest (RetroIO
 * ListRoutingTests.cs: the captured request 102 -> 100 is rebuilt byte for byte):
 *   request   sub-header 86 84, destination = host system : port 0, source = us : our port,
 *             XMCSM 0x0100, body 01 4B 00 04 01 02 <system asked>
 *   reply     sub-header 86 60, both address fields = us : our port, XMCSM 0x0100,
 *             body 01 00 00 10 + p1..p4 (xrout_route.h)
 * Each request is an ordinary data frame with its own Flags1 from our count to the host. The
 * host's reply carries the HOST's own number, not ours (capture list-routing-info-100-102-then-
 * 102-100: request 0x012B, reply 0x00F4), so a reply is matched to its question by being the only
 * question out: one request at a time, and a reply whose system is below the one asked (a late
 * answer to an earlier question) is ignored.
 *
 * Use:
 *   static XroutWalk w;                        (lives until on_done)
 *   XroutWalkCallbacks cb = {ctx, on_entry, on_done};
 *   rc = xrout_walk_start(&w, &node, host_node, host_system, 10000, &cb, now_ms);
 * then keep calling xmsg_node_tick() / feeding the node as usual. The walk uses one session slot
 * (XMSG_MAX_SESSIONS) while it runs and frees it before on_done is called, so on_done may start
 * another walk.
 */
#ifndef XMSG_XROUT_WALK_H
#define XMSG_XROUT_WALK_H

#include <stdbool.h>
#include <stdint.h>

#include "libxmsg/xmsg/xmsg_node.h"
#include "libxmsg/xmsg/xmsg_session.h"
#include "libxmsg/xrout/xrout_route.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Most questions one walk asks before it gives up (a table that never ends is a fault). */
#define XROUT_WALK_MAX_STEPS 1024u

/** Times one question is sent again after a XENSE refusal or a peer restart. */
#define XROUT_WALK_MAX_RESENDS 3u

/** Where a walk is. */
typedef enum XroutWalkState
{
    XROUT_WALK_IDLE = 0, /**< not started, or finished */
    XROUT_WALK_SEND,     /**< a question is due; waiting for a window slot */
    XROUT_WALK_WAIT      /**< the question is out; waiting for the reply */
} XroutWalkState;

/** What the walk tells the application. Any pointer may be NULL. */
typedef struct XroutWalkCallbacks
{
    void *ctx;
    /** One entry of the host's table, in ascending system order. */
    void (*on_entry)(void *ctx, const XroutRouteEntry *e);
    /**
     * The walk ended. `result`: XMSG_OK (the host said "no more"), XMSG_ETIMEDOUT (no reply in
     * timeout_ms, or no window slot in that time), XMSG_EPROTO (the host refused with an XROUT
     * status or an XMSG error, or the table did not end in XROUT_WALK_MAX_STEPS), XMSG_ESTATE
     * (cancelled). `entries` is how many on_entry calls there were.
     */
    void (*on_done)(void *ctx, int result, unsigned entries);
} XroutWalkCallbacks;

/** One walk. Embeds the session; keep it alive until on_done. */
typedef struct XroutWalk
{
    XmsgSession base; /**< first member: the node hands this pointer back */
    XroutWalkCallbacks cb;
    XroutWalkState state;
    uint16_t host_system;
    uint16_t asked;       /**< the system of the question in hand */
    uint16_t sent_flags1; /**< its Flags1, to match a refusal to it */
    uint32_t since_ms;    /**< when the question became due, or went out */
    uint32_t timeout_ms;
    unsigned steps;
    unsigned entries;
    uint8_t resends;
    uint8_t last_status; /**< the XROUT status byte of a refused reply */
    int16_t last_error;  /**< the XMSG error code of a refusal frame */
} XroutWalk;

/**
 * @brief Start walking a host's routing table.
 * @param w           The walk (not running).
 * @param n           The node linked to the host; must be ready (xmsg_node_is_ready()).
 * @param host_node   The host's node number (the LAPB peer).
 * @param host_system Its XMSG system number (the same number on every captured machine).
 * @param timeout_ms  How long one question may wait for its reply; 0 means 10000.
 * @param cb          Callbacks; copied.
 * @param now_ms      Time.
 * @return XMSG_OK; XMSG_EINVAL (NULL); XMSG_EAGAIN (this walk is already running); XMSG_ESTATE
 *         (the link is not ready); XMSG_ENOSPC (every session slot is taken).
 */
int xrout_walk_start(XroutWalk *w, XmsgNode *n, uint16_t host_node, uint16_t host_system,
                     uint32_t timeout_ms, const XroutWalkCallbacks *cb, uint32_t now_ms);

/**
 * @brief Stop a running walk; on_done is called with XMSG_ESTATE.
 * @return XMSG_OK, or XMSG_ESTATE when it was not running.
 */
int xrout_walk_cancel(XroutWalk *w);

/** @brief Whether the walk is running. */
bool xrout_walk_is_busy(const XroutWalk *w);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_XROUT_WALK_H */
