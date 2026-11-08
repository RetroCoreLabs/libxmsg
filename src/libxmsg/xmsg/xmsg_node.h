/*
 * xmsg_node.h - one XMSG node: a link to a peer machine and the sessions running over it.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * The node owns the whole stack below the protocols: the byte framer (optional), the LAPB link,
 * the datagram numbers per peer, fragment reassembly, port words, the acknowledgement duty and
 * the transmit pump. Applications call three things: feed received bytes, tick with the time,
 * and attach sessions. Everything else happens in callbacks.
 *
 * Datagram duties, all measured against real ND machines (see the C# XmsgNode / XmsgServerHost):
 *  - EVERY data frame is answered with a 14-byte ACK echoing its Flags1; without ACKs the peer
 *    retransmits and finally fails the session with XEIMA.
 *  - a reachability request (0x19) from the peer zeroes our count to it and is answered with a
 *    reply (0x13); our own request never touches our count;
 *  - an error frame carrying XENSE steps our count down (xmsg_flags1.h) and the sessions on that
 *    peer are told; XEIMA and other codes are only reported;
 *  - INNAK (0x17) is logged and counted - what it asks for is not established;
 *  - after the link comes up, no data is sent until it has been quiet for `settle_ms`: the ND
 *    kernel sends bursts of SABMs at start-up and a frame between two resets draws an FRMR.
 *
 * Sending: a session asks for the staging buffer, writes its body (up to XMSG_DG_MAX_BODY), and
 * commits with the sub-header fields. The node allocates the Flags1, builds one frame - or a first
 * fragment and a continuation when the body is longer than 594 bytes - into LAPB window slots, and
 * the pump frames and writes them as the transport accepts bytes. The body is NOT padded here;
 * protocols that need word alignment (file access) pad before committing.
 *
 * Threads: none. Every call must come from the same thread (or loop) that calls tick.
 */
#ifndef XMSG_NODE_H
#define XMSG_NODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libxmsg/hdlc/hdlc_async.h"
#include "libxmsg/lapb/lapb.h"
#include "libxmsg/xmsg/xmsg_dg.h"
#include "libxmsg/xmsg/xmsg_flags1.h"
#include "libxmsg/xmsg/xmsg_frag.h"
#include "libxmsg/xmsg/xmsg_portword.h"
#include "libxmsg/xmsg/xmsg_session.h"
#include "libxmsg/xmsg_config.h"
#include "libxmsg/xmsg_errors.h"
#include "libxmsg/xmsg_ports.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** How the transport carries frames. */
typedef enum XmsgFraming
{
    XMSG_FRAMING_ASYNC = 0, /**< RFC 1662 byte stream: TCP bridge, USB serial gateway */
    XMSG_FRAMING_RAW = 1    /**< whole frames: line hardware that frames and checks the FCS */
} XmsgFraming;

/** Short frames (ACK, reachability reply) waiting for a window slot. */
#define XMSG_NODE_PENDING_SHORT 32

/** Recent Flags1 values received per peer, for repeat detection. */
#define XMSG_NODE_RECENT_RX 4

/** Node parameters. xmsg_node_config_default() fills the measured defaults. */
typedef struct XmsgNodeConfig
{
    uint16_t own_node;       /**< our LAPB node number; also the default system number */
    uint16_t own_system;     /**< our XMSG system number (0 = same as own_node) */
    LapbConfig lapb;         /**< link parameters; own_node is copied into it */
    uint32_t settle_ms;      /**< quiet time after link-up before data may flow; 500 */
    XmsgFraming framing;     /**< how the transport carries frames */
    bool transport_adds_fcs; /**< raw framing only: the hardware appends and checks the FCS */
    uint32_t hdlc_accm; /**< async framing: HDLC_ACCM_NONE (nd100x) or ALL_CONTROL (USB gateway) */
    uint16_t portword_seed; /**< seed for minting port words */
    uint8_t persist_every;  /**< Flags1 store policy; 1 = save every frame */
} XmsgNodeConfig;

/** What the node tells the application. Any pointer may be NULL. */
typedef struct XmsgNodeCallbacks
{
    void *ctx;
    /** The link changed state. `up` = LAPB usable; `ready` = up and settled, data may flow. */
    void (*link_state)(void *ctx, LapbState state, bool up, bool ready, const char *reason);
    /** The peer announced a (re)start; our count to it is zero now. */
    void (*peer_reach)(void *ctx, uint16_t peer_node);
    /** A data frame for port 0 (a letter for a named service): the XROUT layer handles these. */
    void (*on_letter)(void *ctx, const XmsgDatagram *dg, uint32_t now_ms);
    /** A data frame no session claimed. */
    void (*unhandled)(void *ctx, const XmsgDatagram *dg);
} XmsgNodeCallbacks;

/** A short frame waiting to be sent. */
typedef struct XmsgPendingShort
{
    uint8_t subtype;
    uint16_t dst_node;
    uint16_t flags1;
    uint16_t flags2;
} XmsgPendingShort;

/** Recently received Flags1 from one peer, for repeat detection. */
typedef struct XmsgRecentRx
{
    bool in_use;
    uint16_t node;
    uint16_t flags1[XMSG_NODE_RECENT_RX];
    uint8_t count;
    uint8_t head;
} XmsgRecentRx;

/** Node statistics. */
typedef struct XmsgNodeStats
{
    uint32_t datagrams_in;
    uint32_t datagrams_out;
    uint32_t acks_sent;
    uint32_t acks_received;
    uint32_t bad_headers; /**< frames whose header checksum or layout was wrong */
    uint32_t repeats;     /**< data frames seen before (same peer, same Flags1) */
    uint32_t errors_in;   /**< 0x07 frames */
    uint32_t xense_in;
    uint32_t innak_in;
    uint32_t reach_in;
    uint32_t unhandled;
    uint32_t short_queue_full;
} XmsgNodeStats;

/** The node. Large: keep it static or in a long-lived struct, never on a small stack. */
struct XmsgNode
{
    XmsgNodeConfig cfg;
    XmsgPorts ports;
    XmsgNodeCallbacks cb;

    /* framing */
    HdlcDecoder rx;
    HdlcEncoder tx;
    uint8_t rx_buf[XMSG_LAPB_N1 + 4];
    uint8_t tx_frame[XMSG_LAPB_N1 + 2];             /**< the frame being sent */
    uint8_t tx_scratch[2 * (XMSG_LAPB_N1 + 4) + 2]; /**< its encoded bytes */
    size_t tx_len;                                  /**< encoded bytes in tx_scratch */
    size_t tx_sent;                                 /**< of which already accepted */
    bool tx_frame_open;                             /**< a frame is being encoded/sent */

    /* link and datagram state */
    LapbLink lapb;
    XmsgFlags1Table flags1;
    XmsgPortwordAlloc portwords;
    XmsgReassembler reasm;
    XmsgPendingShort pending_short[XMSG_NODE_PENDING_SHORT];
    uint8_t pending_head;
    uint8_t pending_count;
    XmsgRecentRx recent[XMSG_MAX_PEERS];     /**< received Flags1, for repeat detection */
    XmsgRecentRx recent_ack[XMSG_MAX_PEERS]; /**< acknowledged Flags1, so a fragmented
                                                  message's two ACKs are reported once */
    bool pumping;                            /**< the pump is running: no re-entry */

    /* the staging buffer a session builds its body in */
    uint8_t stage[XMSG_DG_MAX_BODY];
    bool stage_open;

    /* sessions */
    XmsgSession *sessions[XMSG_MAX_SESSIONS];
    uint8_t next_session_poll; /**< round robin start */

    /* link readiness */
    uint32_t rx_now_ms; /**< the time of the feed call, for the LAPB callbacks */
    bool link_up;
    bool ready;
    uint32_t quiet_since_ms; /**< last LAPB frame of any kind, for the settle timer */
    bool quiet_valid;

    XmsgNodeStats stats;
};

/**
 * @brief Fill the defaults: settle 500 ms, async framing, no ACCM escaping, persist every frame,
 *        LAPB defaults for `own_node`.
 */
void xmsg_node_config_default(XmsgNodeConfig *cfg, uint16_t own_node);

/**
 * @brief Set up a node. Installs the log sink from `ports`.
 * @return XMSG_OK or XMSG_EINVAL.
 */
int xmsg_node_init(XmsgNode *n, const XmsgNodeConfig *cfg, const XmsgPorts *ports,
                   const XmsgNodeCallbacks *cb);

/** @brief Start the link (SABM). */
int xmsg_node_link_start(XmsgNode *n, uint32_t now_ms);

/** @brief Take the link down politely (DISC). */
int xmsg_node_link_stop(XmsgNode *n, uint32_t now_ms);

/** @brief The transport closed under us: drop everything, go to DISCONNECTED without sending. */
void xmsg_node_transport_lost(XmsgNode *n);

/**
 * @brief Feed received bytes (async framing).
 * @return XMSG_OK, or XMSG_ESTATE when the node uses raw framing.
 */
int xmsg_node_feed(XmsgNode *n, const uint8_t *bytes, size_t len, uint32_t now_ms);

/**
 * @brief Feed one received frame (raw framing): address .. info, with FCS when the hardware did
 *        not strip it (cfg.transport_adds_fcs false means the FCS is present and checked here).
 */
int xmsg_node_feed_frame(XmsgNode *n, const uint8_t *frame, size_t len, uint32_t now_ms);

/** @brief Run timers, retry pending output, poll sessions. Call at least every 100 ms. */
int xmsg_node_tick(XmsgNode *n, uint32_t now_ms);

/** @brief LAPB usable. */
bool xmsg_node_is_up(const XmsgNode *n);

/** @brief Up and settled: data may be sent. */
bool xmsg_node_is_ready(const XmsgNode *n);

/** @brief Our XMSG system number. */
uint16_t xmsg_node_system(const XmsgNode *n);

/**
 * @brief Attach a session: it gets a port word and starts receiving.
 * @return XMSG_OK, XMSG_EINVAL, or XMSG_ENOSPC when XMSG_MAX_SESSIONS are attached.
 */
int xmsg_node_attach(XmsgNode *n, XmsgSession *s);

/** @brief Detach a session. */
int xmsg_node_detach(XmsgNode *n, XmsgSession *s);

/**
 * @brief Get the staging buffer to build a datagram body in.
 * @param body Receives the buffer.
 * @param cap  Receives XMSG_DG_MAX_BODY.
 * @return XMSG_OK; XMSG_ESTATE when the link is not ready; XMSG_EAGAIN when a send is already
 *         open or the window has no free slot.
 */
int xmsg_node_send_begin(XmsgNode *n, uint8_t **body, size_t *cap);

/**
 * @brief Commit the staged body as a datagram to a peer: allocates the Flags1, fragments if
 *        needed, and queues the frame(s) in the window.
 * @param peer_node   The peer.
 * @param sub         Sub-header fields; src_system is filled in by the node.
 * @param body_len    Bytes written to the staging buffer; 1..XMSG_DG_MAX_BODY.
 * @param flags1_used Receives the number used (NULL allowed).
 * @return XMSG_OK; XMSG_EAGAIN when the window lacks the one or two slots needed (the staged body
 *         stays, try again later); XMSG_EINVAL; XMSG_ESTATE with no open send.
 */
int xmsg_node_send_commit(XmsgNode *n, uint16_t peer_node, const XmsgSubhdr *sub, size_t body_len,
                          uint32_t now_ms, uint16_t *flags1_used);

/**
 * @brief Like xmsg_node_send_commit() but at a given Flags1: a retransmission of something the
 *        peer asked for again (a file server replaying a reply). Allocates nothing.
 */
int xmsg_node_send_repeat(XmsgNode *n, uint16_t peer_node, const XmsgSubhdr *sub, size_t body_len,
                          uint16_t flags1, uint32_t now_ms);

/** @brief Give up an open send. */
void xmsg_node_send_cancel(XmsgNode *n);

/** @brief Send a reachability request (our restart announcement). Does not touch our count. */
int xmsg_node_send_reach(XmsgNode *n, uint16_t peer_node, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_NODE_H */
