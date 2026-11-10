/*
 * xrout_names_walk.h - read a remote XROUT's name table: XSGNI asked again and again, the
 *                      client side.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * This is what LIST-NAMES does on a SINTRAN machine: ask the remote XROUT (port 0) "first row at
 * or above magic 0", then "at or above (previous magic + 1)", and so on until the end form
 * (magic 0). Every row found is handed to on_row; at the end on_done says how it went. XMSGGW
 * uses it at link-up to learn D100's system names (aliases) and named ports. Nothing blocks: the
 * walk is a session on the node and moves on node ticks and received frames.
 *
 * Wire form: xrout_name.h, from the complete walk captured 2026-11-10 on the COSMOS hub
 * (listnames-hub.pcapng, D102 reading D100):
 *   request   sub-header 86 84, destination = remote system : port 0, source = us : our port,
 *             XMCSM 0x0100, body 01 45 00 04 01 02 00 00 first, then 01 45 00 06 01 04 <magic32>
 *   reply     sub-header 86 60, both address fields = us : our port, XMCSM 0x0100
 * Each request is an ordinary data frame on our own Flags1 count to the link neighbour; the reply
 * comes on the neighbour's own count (D100's replies 0x0011.. against 102's requests 0x0012..),
 * so a reply is matched to its question by being the only question out: one request at a time,
 * and a reply whose magic is below the one asked (a late answer to an earlier question) is
 * ignored. The rules are the routing walk's (xrout_walk.h), which this copies.
 *
 * Use:
 *   static XroutNamesWalk w;                     (lives until on_done)
 *   XroutNamesWalkCallbacks cb = {ctx, on_row, on_done};
 *   rc = xrout_names_walk_start(&w, &node, link_node, remote_system, 10000, &cb, now_ms);
 * then keep calling xmsg_node_tick() / feeding the node as usual. The walk uses one session slot
 * (XMSG_MAX_SESSIONS) while it runs and frees it before on_done is called, so on_done may start
 * another walk.
 */
#ifndef XMSG_XROUT_NAMES_WALK_H
#define XMSG_XROUT_NAMES_WALK_H

#include <stdbool.h>
#include <stdint.h>

#include "libxmsg/xmsg/xmsg_node.h"
#include "libxmsg/xmsg/xmsg_session.h"
#include "libxmsg/xrout/xrout_name.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Most questions one walk asks before it gives up (a table that never ends is a fault). */
#define XROUT_NAMES_WALK_MAX_STEPS 1024u

/** Times one question is sent again after a XENSE refusal or a peer restart. */
#define XROUT_NAMES_WALK_MAX_RESENDS 3u

/** Where a walk is. */
typedef enum XroutNamesWalkState
{
    XROUT_NAMES_WALK_IDLE = 0, /**< not started, or finished */
    XROUT_NAMES_WALK_SEND,     /**< a question is due; waiting for a window slot */
    XROUT_NAMES_WALK_WAIT      /**< the question is out; waiting for the reply */
} XroutNamesWalkState;

/** What the walk tells the application. Any pointer may be NULL. */
typedef struct XroutNamesWalkCallbacks
{
    void *ctx;
    /**
     * One row of the remote table, in ascending magic order: system, port word (0x0001 = a
     * system name), name, and free seats (XROUT_NAME_NO_FREE when the reply had none). The row
     * is only valid during the call.
     */
    void (*on_row)(void *ctx, const XroutNameRow *row);
    /**
     * The walk ended. `result`: XMSG_OK (the end form came), XMSG_ETIMEDOUT (no reply in
     * timeout_ms, or no window slot in that time), XMSG_EPROTO (a refusal with an XROUT status
     * or an XMSG error, a reply that does not parse, or no end in XROUT_NAMES_WALK_MAX_STEPS),
     * XMSG_ESTATE (cancelled). `rows` is how many on_row calls there were.
     */
    void (*on_done)(void *ctx, int result, unsigned rows);
} XroutNamesWalkCallbacks;

/** One walk. Embeds the session; keep it alive until on_done. */
typedef struct XroutNamesWalk
{
    XmsgSession base; /**< first member: the node hands this pointer back */
    XroutNamesWalkCallbacks cb;
    XroutNamesWalkState state;
    uint16_t remote_system;
    uint32_t asked;       /**< the magic of the question in hand */
    uint16_t sent_flags1; /**< its Flags1, to match a refusal to it */
    uint32_t since_ms;    /**< when the question became due, or went out */
    uint32_t timeout_ms;
    unsigned steps;
    unsigned rows;
    uint8_t resends;
    uint8_t last_status; /**< the XROUT status byte of a refused reply */
    int16_t last_error;  /**< the XMSG error code of a refusal frame */
} XroutNamesWalk;

/**
 * @brief Start reading a remote XROUT's name table.
 * @param w             The walk (not running).
 * @param n             The node; must be ready (xmsg_node_is_ready()).
 * @param link_node     The node the questions are ADDRESSED to (the SINTRAN header's destination
 *                      node; replies come from it). For a system that is not the link neighbour
 *                      this is that system's own node, not the neighbour's: the neighbour relays
 *                      it. MEASURED 2026-11-10 22:06 (D102 LIST-NAMES of D101 through D100, hub +
 *                      HDLC capture): D102 sent header dst 0x0065, sub-header dst system 0x0065,
 *                      and D100 passed it on with hop 0x12. Addressed to the neighbour (0x0064)
 *                      with system 0x0066 in the sub-header, D100 answered XEIMA itself.
 *                      The frame always leaves over our one link.
 * @param remote_system The XMSG system whose XROUT is asked (the destination system field).
 * @param timeout_ms    How long one question may wait for its reply; 0 means 10000.
 * @param cb            Callbacks; copied.
 * @param now_ms        Time.
 * @return XMSG_OK; XMSG_EINVAL (NULL); XMSG_EAGAIN (this walk is already running); XMSG_ESTATE
 *         (the link is not ready); XMSG_ENOSPC (every session slot is taken).
 */
int xrout_names_walk_start(XroutNamesWalk *w, XmsgNode *n, uint16_t link_node,
                           uint16_t remote_system, uint32_t timeout_ms,
                           const XroutNamesWalkCallbacks *cb, uint32_t now_ms);

/**
 * @brief Stop a running walk; on_done is called with XMSG_ESTATE.
 * @return XMSG_OK, or XMSG_ESTATE when it was not running.
 */
int xrout_names_walk_cancel(XroutNamesWalk *w);

/** @brief Whether the walk is running. */
bool xrout_names_walk_is_busy(const XroutNamesWalk *w);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_XROUT_NAMES_WALK_H */
