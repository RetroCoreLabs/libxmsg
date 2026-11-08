/*
 * xmsg_session.h - what every protocol on top of the node (TAD, file access, ...) implements.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * A session is one conversation with one port on one peer machine. The node owns the link, the
 * datagram numbers and the acknowledgements; a session only sees the datagrams addressed to its
 * port, is asked for something to send when the window has room, and is told when its frames
 * were acknowledged or refused. Sessions embed this struct as their FIRST member so the node can
 * hand the pointer back to them.
 *
 * Matching: an incoming data frame goes to the attached session whose local port word equals the
 * frame's destination port, and whose peer node matches, and whose peer port (when already
 * learned) equals the frame's source port. ACKs and errors carry no ports, so they go to every
 * session on that peer; a session matches them against the Flags1 it sent.
 */
#ifndef XMSG_SESSION_H
#define XMSG_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include "libxmsg/xmsg/xmsg_dg.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct XmsgNode XmsgNode;
typedef struct XmsgSession XmsgSession;

/** The calls a session receives. Any pointer may be NULL. */
typedef struct XmsgSessionOps
{
    /** A data datagram for this session's port. `dg->repeat` is set for a retransmission. */
    void (*on_datagram)(XmsgSession *s, const XmsgDatagram *dg, uint32_t now_ms);
    /** A window slot is free: build and commit at most one datagram. Return 1 if one was sent,
     *  0 if nothing was pending. Called only while `wants_tx` is set. */
    int (*poll_tx)(XmsgSession *s, uint32_t now_ms);
    /** Time passes. */
    void (*tick)(XmsgSession *s, uint32_t now_ms);
    /** The peer acknowledged a datagram with this Flags1 (any session on that peer is told). */
    void (*on_delivered)(XmsgSession *s, uint16_t flags1);
    /** The peer refused a datagram: `code` is the XMSG error (XENSE -34, XEIMA -19, ...). */
    void (*on_error)(XmsgSession *s, int16_t code, uint16_t flags1);
} XmsgSessionOps;

/** The part of a session the node sees. */
struct XmsgSession
{
    const XmsgSessionOps *ops;
    XmsgNode *node;       /**< set by xmsg_node_attach() */
    uint16_t peer_node;   /**< the machine at the other end (LAPB node number) */
    uint16_t peer_system; /**< its XMSG system number; 0 = accept any */
    uint16_t local_port;  /**< our port word, minted at attach */
    uint16_t peer_port;   /**< its port word once learned; 0 = not yet, accept any */
    bool any_peer_port;   /**< take frames from every port of the peer and learn none (a TAD
                           *   host answers from its *TADADM port AND its session port) */
    bool attached;
    bool wants_tx; /**< set by the session when it has something to send */
};

#ifdef __cplusplus
}
#endif

#endif /* XMSG_SESSION_H */
