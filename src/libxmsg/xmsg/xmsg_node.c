/*
 * xmsg_node.c - the node: framer, link, datagram numbers, acknowledgements, sessions, pump.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/xmsg/xmsg_node.h"

#include <string.h>

#include "libhdlc/hdlc.h"
#include "libxmsg/log/xmsg_log.h"

static void node_pump(XmsgNode *n, uint32_t now_ms);

/* ------------------------------------------------------------------------------------------ */
/* Set-up                                                                                     */
/* ------------------------------------------------------------------------------------------ */

void xmsg_node_config_default(XmsgNodeConfig *cfg, uint16_t own_node)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->own_node = own_node;
    cfg->own_system = own_node;
    lapb_config_default(&cfg->lapb, own_node);
    cfg->settle_ms = 500;
    /* Chosen, not measured: well above the LAPB T1 (3000 ms) that a lost request frame would
     * already have been resent within; the host's reply time itself has not been measured. */
    cfg->resync_timeout_ms = 5000;
    cfg->framing = XMSG_FRAMING_ASYNC;
    cfg->transport_adds_fcs = false;
    cfg->hdlc_accm = BYTE_FRAME_ACCM_NONE;
    cfg->portword_seed = 0x5A5A;
    cfg->persist_every = 1;
    cfg->answer_routing = true;
    cfg->reject_closed_ports = true;
    /* The C# Topology's default link index (TopologyNode.Link = 1); real neighbours report 1 or
     * 2 (li-rout-103-tree, li-route-d103-tree). It is our own numbering of our own links. */
    cfg->route_link_index = 1;
}

static void notify_link(XmsgNode *n, LapbState state, const char *reason)
{
    if (n->cb.link_state != NULL)
    {
        n->cb.link_state(n->cb.ctx, state, n->link_up, n->ready, reason);
    }
}

static void on_lapb_state(void *ctx, LapbState state, bool up, const char *reason)
{
    XmsgNode *n = (XmsgNode *)ctx;
    bool was_up = n->link_up;
    n->link_up = up;
    if (!up)
    {
        n->ready = false;
        n->stage_open = false;
        n->pending_count = 0;
        n->pending_route_count = 0;
        /* A reachability reply can no longer come on this link: stop waiting for it, or every
         * later xmsg_node_resync() to that peer would send nothing (found by the XMSGGW port,
         * 2026-11-09). */
        n->resync_pending = false;
    }
    if (up && !was_up)
    {
        /* The settle clock starts now; data waits until the link has been quiet. */
        n->quiet_valid = false;
    }
    XMSG_LOG(XMSG_LOG_NODE, XMSG_LOG_INFO, "link %s up=%d: %s", lapb_state_name(state), up ? 1 : 0,
             reason);
    notify_link(n, state, reason);
}

static void on_lapb_info(void *ctx, const uint8_t *info, size_t len);

int xmsg_node_init(XmsgNode *n, const XmsgNodeConfig *cfg, const XmsgPorts *ports,
                   const XmsgNodeCallbacks *cb)
{
    if (n == NULL || cfg == NULL || ports == NULL || ports->transport.write == NULL)
    {
        return XMSG_EINVAL;
    }
    memset(n, 0, sizeof *n);
    n->cfg = *cfg;
    if (n->cfg.own_system == 0)
    {
        n->cfg.own_system = n->cfg.own_node;
    }
    n->cfg.lapb.own_node = n->cfg.own_node;
    n->ports = *ports;
    if (cb != NULL)
    {
        n->cb = *cb;
    }
    xmsg_log_set_sink(&n->ports.log);

    if (byte_frame_rx_init(&n->rx, n->rx_buf, sizeof n->rx_buf, true) != 0)
    {
        return XMSG_EINVAL;
    }
    byte_frame_tx_init(&n->tx, n->cfg.hdlc_accm, true);
    if (lapb_init(&n->lapb, &n->cfg.lapb, n, on_lapb_info, on_lapb_state) != XMSG_OK)
    {
        return XMSG_EINVAL;
    }
    xmsg_flags1_init(&n->flags1, &n->ports.flags1, n->cfg.persist_every);
    uint16_t seed = n->cfg.portword_seed;
    if (n->ports.flags1.load_port_seed != NULL &&
        n->ports.flags1.load_port_seed(n->ports.flags1.ctx, &seed) != 0)
    {
        seed = n->cfg.portword_seed;
    }
    xmsg_portword_init(&n->portwords, seed);
    xmsg_reasm_init(&n->reasm);
    return XMSG_OK;
}

int xmsg_node_link_start(XmsgNode *n, uint32_t now_ms)
{
    int rc = lapb_connect(&n->lapb, now_ms);
    node_pump(n, now_ms);
    return rc;
}

int xmsg_node_link_stop(XmsgNode *n, uint32_t now_ms)
{
    int rc = lapb_disconnect(&n->lapb, now_ms);
    node_pump(n, now_ms);
    return rc;
}

void xmsg_node_transport_lost(XmsgNode *n)
{
    lapb_force_down(&n->lapb);
    byte_frame_rx_reset(&n->rx);
    byte_frame_tx_init(&n->tx, n->cfg.hdlc_accm, true);
    n->tx_len = 0;
    n->tx_sent = 0;
    n->tx_frame_open = false;
    n->pending_count = 0;
    n->stage_open = false;
    xmsg_reasm_init(&n->reasm);
}

bool xmsg_node_is_up(const XmsgNode *n)
{
    return n->link_up;
}

bool xmsg_node_is_ready(const XmsgNode *n)
{
    return n->ready;
}

uint16_t xmsg_node_system(const XmsgNode *n)
{
    return n->cfg.own_system;
}

/* ------------------------------------------------------------------------------------------ */
/* Sessions                                                                                   */
/* ------------------------------------------------------------------------------------------ */

int xmsg_node_attach(XmsgNode *n, XmsgSession *s)
{
    if (n == NULL || s == NULL || s->ops == NULL)
    {
        return XMSG_EINVAL;
    }
    for (int i = 0; i < XMSG_MAX_SESSIONS; i++)
    {
        if (n->sessions[i] == NULL)
        {
            n->sessions[i] = s;
            s->node = n;
            s->attached = true;
            /* Port index i + 1: one-based, and index 0 is XROUT's. */
            s->local_port = xmsg_portword_next(&n->portwords, (uint16_t)(i + 1));
            if (n->ports.flags1.save_port_seed != NULL &&
                n->ports.flags1.save_port_seed(n->ports.flags1.ctx, n->portwords.seed) != 0)
            {
                XMSG_LOG(XMSG_LOG_NODE, XMSG_LOG_WARN, "port seed not saved");
            }
            XMSG_LOG(XMSG_LOG_NODE, XMSG_LOG_DEBUG, "session %d attached, port word 0x%04X", i,
                     s->local_port);
            return XMSG_OK;
        }
    }
    return XMSG_ENOSPC;
}

int xmsg_node_detach(XmsgNode *n, XmsgSession *s)
{
    for (int i = 0; i < XMSG_MAX_SESSIONS; i++)
    {
        if (n->sessions[i] == s)
        {
            n->sessions[i] = NULL;
            s->attached = false;
            s->node = NULL;
            return XMSG_OK;
        }
    }
    return XMSG_EINVAL;
}

static XmsgSession *find_session(XmsgNode *n, const XmsgDatagram *dg)
{
    for (int i = 0; i < XMSG_MAX_SESSIONS; i++)
    {
        XmsgSession *s = n->sessions[i];
        if (s == NULL || s->local_port != dg->sub.dst_port)
        {
            continue;
        }
        if (s->peer_node != dg->hdr.src_node)
        {
            continue;
        }
        if (s->peer_system != 0 && s->peer_system != dg->sub.src_system)
        {
            continue;
        }
        if (s->peer_port != 0 && s->peer_port != dg->sub.src_port)
        {
            continue;
        }
        return s;
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------------ */
/* Short frames and repeat detection                                                          */
/* ------------------------------------------------------------------------------------------ */

static void queue_short(XmsgNode *n, uint8_t subtype, uint16_t dst_node, uint16_t flags1,
                        uint16_t flags2)
{
    if (n->pending_count >= XMSG_NODE_PENDING_SHORT)
    {
        /* Our window has been stuck behind lost frames for so long that the peer delivered more
         * than the queue holds. Dropping the ACK is safe: XMSG retransmits an unacknowledged
         * datagram, the repeat arrives flagged, and is acknowledged then. (Telling the peer RNR
         * here was tried and deadlocks when both sides do it, because the ACKs themselves ride
         * in I-frames; see the P3 notes.) */
        n->stats.short_queue_full++;
        XMSG_LOG(XMSG_LOG_NODE, XMSG_LOG_WARN, "short-frame queue full: %s to node %u dropped",
                 xmsg_dg_subtype_name(subtype), dst_node);
        return;
    }
    uint8_t at = (uint8_t)((n->pending_head + n->pending_count) % XMSG_NODE_PENDING_SHORT);
    n->pending_short[at].subtype = subtype;
    n->pending_short[at].dst_node = dst_node;
    n->pending_short[at].flags1 = flags1;
    n->pending_short[at].flags2 = flags2;
    n->pending_count++;
}

/* True when some attached session owns this local port. Only the port is asked, not the peer
 * port: a real node rejects on the magic number (our port word) alone. */
static bool port_is_open(const XmsgNode *n, uint16_t port)
{
    for (int i = 0; i < XMSG_MAX_SESSIONS; i++)
    {
        if (n->sessions[i] != NULL && n->sessions[i]->local_port == port)
        {
            return true;
        }
    }
    return false;
}

/* Record a Flags1 seen from a peer in a small per-peer ring; true when it was already there. */
static bool note_seen(XmsgRecentRx *table, uint16_t node, uint16_t flags1)
{
    XmsgRecentRx *r = NULL;
    for (int i = 0; i < XMSG_MAX_PEERS; i++)
    {
        if (table[i].in_use && table[i].node == node)
        {
            r = &table[i];
            break;
        }
        if (!table[i].in_use && r == NULL)
        {
            r = &table[i];
        }
    }
    if (r == NULL)
    {
        return false; /* more peers than slots: cannot tell, treat as new */
    }
    if (!r->in_use)
    {
        r->in_use = true;
        r->node = node;
        r->count = 0;
        r->head = 0;
    }
    for (uint8_t i = 0; i < r->count; i++)
    {
        if (r->flags1[i] == flags1)
        {
            return true;
        }
    }
    r->flags1[r->head] = flags1;
    r->head = (uint8_t)((r->head + 1) % XMSG_NODE_RECENT_RX);
    if (r->count < XMSG_NODE_RECENT_RX)
    {
        r->count++;
    }
    return false;
}

/* ------------------------------------------------------------------------------------------ */
/* Receiving                                                                                  */
/* ------------------------------------------------------------------------------------------ */

static void deliver_to_peer_sessions(XmsgNode *n, uint16_t peer_node, bool is_error, int16_t code,
                                     uint16_t flags1)
{
    for (int i = 0; i < XMSG_MAX_SESSIONS; i++)
    {
        XmsgSession *s = n->sessions[i];
        if (s == NULL || s->peer_node != peer_node)
        {
            continue;
        }
        if (is_error)
        {
            if (s->ops->on_error != NULL)
            {
                s->ops->on_error(s, code, flags1);
            }
        }
        else if (s->ops->on_delivered != NULL)
        {
            s->ops->on_delivered(s, flags1);
        }
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Routing questions (XSGSY); the rules are in xmsg_node.h                                    */
/* ------------------------------------------------------------------------------------------ */

static void forget_route_answers(XmsgNode *n, uint16_t peer_node)
{
    for (int i = 0; i < XMSG_NODE_ROUTE_MEMORY; i++)
    {
        if (n->route_answers[i].peer_node == peer_node)
        {
            n->route_answers[i].in_use = false;
        }
    }
}

/* Put the two automatic entries in, unless the application already has one for that system. */
static void route_auto_entries(XmsgNode *n)
{
    XroutRouteEntry e;
    int32_t peer = lapb_peer_node(&n->lapb);
    if (!xrout_route_has(&n->routes, n->cfg.own_system))
    {
        /* Every captured reply about the answering machine itself: 04, own number, 0. */
        e.system = n->cfg.own_system;
        e.how = XROUT_REACH_LOCAL;
        e.detail = n->cfg.own_system;
        e.distance = 0;
        (void)xrout_route_put(&n->routes, &e); /* fails only when the app filled the table */
    }
    if (peer > 0 && (uint16_t)peer != n->cfg.own_system &&
        !xrout_route_has(&n->routes, (uint16_t)peer))
    {
        /* Captured neighbour entries: 01, link index, 1 hop (li-rout-103-tree: 100 about 102). */
        e.system = (uint16_t)peer;
        e.how = XROUT_REACH_NEIGHBOUR;
        e.detail = n->cfg.route_link_index;
        e.distance = 1;
        (void)xrout_route_put(&n->routes, &e);
    }
}

int xmsg_node_route_put(XmsgNode *n, const XroutRouteEntry *e)
{
    if (n == NULL)
    {
        return XMSG_EINVAL;
    }
    return xrout_route_put(&n->routes, e);
}

const XroutRouteTable *xmsg_node_routes(XmsgNode *n)
{
    if (n == NULL)
    {
        return NULL;
    }
    route_auto_entries(n);
    return &n->routes;
}

/* An XSGSY question: XMCSM 0x0100 and service byte 0x4B (XMCSM_XSGSY_REQUEST, tad_wire.h). */
static bool is_route_request(const XmsgDatagram *dg)
{
    return dg->has_subhdr && dg->sub.xmcsm == XROUT_ROUTE_XMCSM && dg->body_len >= 2 &&
           dg->body[1] == XROUT_ROUTE_SERVICE;
}

static bool route_reply_queued(const XmsgNode *n, uint16_t peer_node, uint16_t req_flags1)
{
    for (uint8_t k = 0; k < n->pending_route_count; k++)
    {
        const XmsgPendingRoute *p =
            &n->pending_route[(n->pending_route_head + k) % XMSG_NODE_PENDING_ROUTE];
        if (p->peer_node == peer_node && p->req_flags1 == req_flags1)
        {
            return true;
        }
    }
    return false;
}

static void handle_route_request(XmsgNode *n, const XmsgDatagram *dg)
{
    XmsgPendingRoute p;
    uint16_t asked = 0;
    uint8_t serial = 0;

    n->stats.route_requests++;
    if (xrout_route_request_read(dg->body, dg->body_len, &serial, &asked) != XMSG_OK)
    {
        /* No system number to look up: the C# throws here and sends nothing. Say so loudly. */
        n->stats.route_bad++;
        XMSG_LOG(XMSG_LOG_NODE, XMSG_LOG_WARN,
                 "routing question from node %u F1=0x%04X has no system number (%u bytes): "
                 "not answered",
                 dg->hdr.src_node, dg->hdr.flags1, (unsigned)dg->body_len);
        return;
    }
    memset(&p, 0, sizeof p);
    p.peer_node = dg->hdr.src_node;
    p.req_flags1 = dg->hdr.flags1;
    p.asker_system = dg->sub.src_system;
    p.asker_port = dg->sub.src_port;
    p.serial = serial;

    if (dg->repeat)
    {
        if (route_reply_queued(n, p.peer_node, p.req_flags1))
        {
            return; /* the reply to the first copy has not even gone out yet */
        }
        for (int i = 0; i < XMSG_NODE_ROUTE_MEMORY; i++)
        {
            const XmsgRouteAnswer *a = &n->route_answers[i];
            if (a->in_use && a->peer_node == p.peer_node && a->req_flags1 == p.req_flags1)
            {
                p.is_repeat = true;
                p.reply_flags1 = a->reply_flags1;
                break;
            }
        }
        /* Not remembered (the first copy was dropped, or the memory moved on): a new answer. */
    }

    route_auto_entries(n);
    (void)xrout_route_lookup(&n->routes, asked, &p.entry); /* a miss answers system 0 */
    if (n->pending_route_count >= XMSG_NODE_PENDING_ROUTE)
    {
        /* The host asks again when no reply comes; nothing is lost but time. */
        n->stats.route_bad++;
        XMSG_LOG(XMSG_LOG_NODE, XMSG_LOG_WARN,
                 "routing reply queue full: question F1=0x%04X from node %u not answered",
                 p.req_flags1, p.peer_node);
        return;
    }
    n->pending_route[(n->pending_route_head + n->pending_route_count) % XMSG_NODE_PENDING_ROUTE] =
        p;
    n->pending_route_count++;
    XMSG_LOG(XMSG_LOG_NODE, XMSG_LOG_DEBUG,
             "routing question from node %u F1=0x%04X for system %u%s: answer system %u (%s)",
             p.peer_node, p.req_flags1, asked, p.is_repeat ? " (repeat)" : "", p.entry.system,
             xrout_route_how_name(p.entry.how));
}

/* Send the oldest waiting XSGSY reply. The caller has checked a window slot is free. Returns
 * true when the reply went into the window. */
static bool send_route_reply(XmsgNode *n, uint32_t now_ms)
{
    XmsgPendingRoute *p = &n->pending_route[n->pending_route_head];
    uint8_t body[XROUT_ROUTE_REP_SIZE];
    XmsgSubhdr sub;
    uint16_t flags1;
    uint8_t *info;
    size_t cap;
    size_t written;

    /* Both address fields carry the ASKER's system and port: the reply goes back to the port
     * that asked, and XROUT allocates none of its own (C# ListRoutingServer.cs; every captured
     * reply, e.g. list-routing-info frame 10: 0066 02A5 0066 02A5). */
    sub.frame_flags = XROUT_ROUTE_REP_FRAME_FLAGS;
    sub.role = XROUT_ROUTE_REP_ROLE;
    sub.dst_system = p->asker_system;
    sub.dst_port = p->asker_port;
    sub.src_system = p->asker_system;
    sub.src_port = p->asker_port;
    sub.xmcsm = XROUT_ROUTE_XMCSM;
    xrout_route_reply_write(body, p->serial, &p->entry);

    /* Reserve the slot BEFORE spending a number: a number spent on nothing is a gap the peer
     * would wait for (the same rule as xmsg_node_send_commit). */
    if (lapb_tx_reserve(&n->lapb, &info, &cap) != XMSG_OK)
    {
        return false;
    }
    if (p->is_repeat)
    {
        flags1 = p->reply_flags1;
    }
    else if (xmsg_flags1_allocate(&n->flags1, p->peer_node, &flags1) != XMSG_OK)
    {
        lapb_tx_cancel(&n->lapb);
        return false;
    }
    written = xmsg_dg_write_data(info, cap, (uint8_t)SINTRAN_SUBTYPE_DATA, p->peer_node,
                                 n->cfg.own_node, flags1, XROUT_ROUTE_XMCSM, &sub, body,
                                 sizeof body);
    if (written == 0 || lapb_tx_commit(&n->lapb, written, now_ms) != XMSG_OK)
    {
        lapb_tx_cancel(&n->lapb);
        XMSG_LOG(XMSG_LOG_NODE, XMSG_LOG_ERROR,
                 "routing reply F1=0x%04X could not be built: the number is lost", flags1);
        return false;
    }
    if (!p->is_repeat)
    {
        XmsgRouteAnswer *a = &n->route_answers[n->route_answer_next];
        a->in_use = true;
        a->peer_node = p->peer_node;
        a->req_flags1 = p->req_flags1;
        a->reply_flags1 = flags1;
        n->route_answer_next = (uint8_t)((n->route_answer_next + 1) % XMSG_NODE_ROUTE_MEMORY);
    }
    n->stats.datagrams_out++;
    n->stats.route_replies++;
    XMSG_LOG(XMSG_LOG_DG, XMSG_LOG_TRACE, "TX ROUTING REPLY to %u F1=0x%04X for question 0x%04X",
             p->peer_node, flags1, p->req_flags1);
    n->pending_route_head = (uint8_t)((n->pending_route_head + 1) % XMSG_NODE_PENDING_ROUTE);
    n->pending_route_count--;
    return true;
}

static void handle_datagram(XmsgNode *n, const XmsgDatagram *in, uint32_t now_ms)
{
    XmsgDatagram whole;
    int rc;

    n->stats.datagrams_in++;

    switch (in->hdr.subtype)
    {
    case SINTRAN_SUBTYPE_ACK:
        n->stats.acks_received++;
        xmsg_flags1_on_delivered(&n->flags1, in->hdr.src_node);
        /* A fragmented message is acknowledged per fragment, both with the same Flags1; the
         * sessions hear about each number once. */
        if (!note_seen(n->recent_ack, in->hdr.src_node, in->hdr.flags1))
        {
            deliver_to_peer_sessions(n, in->hdr.src_node, false, 0, in->hdr.flags1);
        }
        return;

    case SINTRAN_SUBTYPE_ERROR:
        n->stats.errors_in++;
        {
            int16_t code = (int16_t)in->hdr.flags2;
            if (code == XMSG_XE_XENSE)
            {
                n->stats.xense_in++;
                (void)xmsg_flags1_on_xense(&n->flags1, in->hdr.src_node, in->hdr.flags1);
            }
            else
            {
                XMSG_LOG(XMSG_LOG_NODE, XMSG_LOG_WARN,
                         "error %d (%s) from node %u for Flags1 0x%04X", (int)code,
                         sintran_error_name(code), in->hdr.src_node, in->hdr.flags1);
            }
            deliver_to_peer_sessions(n, in->hdr.src_node, true, code, in->hdr.flags1);
        }
        return;

    case SINTRAN_SUBTYPE_REACH_REQUEST:
        n->stats.reach_in++;
        XMSG_LOG(XMSG_LOG_NODE, XMSG_LOG_INFO, "reachability request from node %u",
                 in->hdr.src_node);
        xmsg_flags1_reset(&n->flags1, in->hdr.src_node);
        forget_route_answers(n, in->hdr.src_node); /* their numbers belong to the old count */
        /* The reply carries the request's own Flags1. RetroIO pcap-decode-sample.json: request
         * 342 (FFFF) -> reply 360 (FFFF), and request 1379 (0028, the sender's next number) ->
         * reply 1451 (0028). A fixed FFFF is right only for the first kind; D100 sends 0000. */
        queue_short(n, (uint8_t)SINTRAN_SUBTYPE_REACH_REPLY, in->hdr.src_node, in->hdr.flags1,
                    0x0001);
        for (int i = 0; i < XMSG_MAX_SESSIONS; i++)
        {
            XmsgSession *s = n->sessions[i];
            if (s != NULL && s->peer_node == in->hdr.src_node && s->ops->on_peer_restart != NULL)
            {
                s->ops->on_peer_restart(s);
            }
        }
        if (n->cb.peer_reach != NULL)
        {
            n->cb.peer_reach(n->cb.ctx, in->hdr.src_node);
        }
        return;

    case SINTRAN_SUBTYPE_REACH_REPLY:
        XMSG_LOG(XMSG_LOG_NODE, XMSG_LOG_INFO, "reachability reply from node %u", in->hdr.src_node);
        if (n->resync_pending && in->hdr.src_node == n->resync_peer)
        {
            /* The peer has started its count for us again at 0 (see xmsg_node_resync): ours
             * follows, and every session to it sends its unacknowledged frame again. */
            n->resync_pending = false;
            n->stats.resyncs++;
            xmsg_flags1_reset(&n->flags1, in->hdr.src_node);
            for (int i = 0; i < XMSG_MAX_SESSIONS; i++)
            {
                XmsgSession *s = n->sessions[i];
                if (s != NULL && s->peer_node == in->hdr.src_node &&
                    s->ops->on_peer_restart != NULL)
                {
                    s->ops->on_peer_restart(s);
                }
            }
        }
        return;

    case SINTRAN_SUBTYPE_INNAK:
        n->stats.innak_in++;
        XMSG_LOG(XMSG_LOG_NODE, XMSG_LOG_WARN,
                 "INNAK from node %u: %02X %02X %02X %02X %04X %04X %04X %04X (meaning not "
                 "established; no action)",
                 in->hdr.src_node, in->hdr.version, in->hdr.hop, in->hdr.type, in->hdr.subtype,
                 in->hdr.dst_node, in->hdr.src_node, in->hdr.flags1, in->hdr.flags2);
        return;

    case SINTRAN_SUBTYPE_DATA:
    case SINTRAN_SUBTYPE_FIRST_FRAG:
    case SINTRAN_SUBTYPE_CONTINUATION:
        /* Data for a port nobody here owns gets a bare 14-byte error frame, Flags2 XEIMA (FFED),
         * at the offending frame's own Flags1, and NO ACK. That is what real ND machines send
         * (10 of 10 rejects in four captures; e.g. NDInsight TRIO-SEQUENCE-2026-08-11
         * trio.pcapng #9333 -> #9349), and the sender then gives up on that conversation.
         * Without it a host waits for ever: D100's TADA hung on a 7WHO to the port of a session
         * lost in a reboot (live 2026-11-10 09:44:39). Port 0 is XROUT's and always open.
         * Continuation frames carry no port and are acknowledged as before. */
        if (n->cfg.reject_closed_ports && in->has_subhdr && in->sub.dst_port != 0 &&
            !port_is_open(n, in->sub.dst_port))
        {
            queue_short(n, (uint8_t)SINTRAN_SUBTYPE_ERROR, in->hdr.src_node, in->hdr.flags1,
                        (uint16_t)XMSG_XE_XEIMA);
            n->stats.closed_port_rejects++;
            XMSG_LOG(XMSG_LOG_NODE, XMSG_LOG_INFO,
                     "data from node %u port 0x%04X to closed port 0x%04X: XEIMA sent",
                     in->hdr.src_node, in->sub.src_port, in->sub.dst_port);
            if (n->cb.unhandled != NULL)
            {
                n->cb.unhandled(n->cb.ctx, in);
            }
            return;
        }
        /* Every other data-carrying frame is acknowledged, fragments included, before anything
         * else. */
        queue_short(n, (uint8_t)SINTRAN_SUBTYPE_ACK, in->hdr.src_node, in->hdr.flags1, 0x0001);
        n->stats.acks_sent++;
        rc = xmsg_reasm_accept(&n->reasm, in, &whole, now_ms);
        if (rc != 1)
        {
            return;
        }
        whole.repeat = note_seen(n->recent, whole.hdr.src_node, whole.hdr.flags1);
        if (whole.repeat)
        {
            n->stats.repeats++;
        }
        if (whole.sub.dst_port == 0)
        {
            if (n->cfg.answer_routing && is_route_request(&whole))
            {
                handle_route_request(n, &whole);
                return;
            }
            if (n->cb.on_letter != NULL)
            {
                n->cb.on_letter(n->cb.ctx, &whole, now_ms);
            }
            else
            {
                n->stats.unhandled++;
            }
            return;
        }
        {
            XmsgSession *s = find_session(n, &whole);
            if (s != NULL && s->ops->on_datagram != NULL)
            {
                if (s->peer_port == 0 && !s->any_peer_port)
                {
                    s->peer_port = whole.sub.src_port; /* learned, never computed */
                }
                s->ops->on_datagram(s, &whole, now_ms);
            }
            else
            {
                n->stats.unhandled++;
                XMSG_LOG(XMSG_LOG_NODE, XMSG_LOG_DEBUG,
                         "no session for data from node %u port 0x%04X to port 0x%04X",
                         whole.hdr.src_node, whole.sub.src_port, whole.sub.dst_port);
                if (n->cb.unhandled != NULL)
                {
                    n->cb.unhandled(n->cb.ctx, &whole);
                }
            }
        }
        return;

    default:
        XMSG_LOG(XMSG_LOG_NODE, XMSG_LOG_WARN, "unknown subtype 0x%02X from node %u",
                 in->hdr.subtype, in->hdr.src_node);
        return;
    }
}

static void on_lapb_info(void *ctx, const uint8_t *info, size_t len)
{
    XmsgNode *n = (XmsgNode *)ctx;
    XmsgDatagram dg;
    int rc = xmsg_dg_parse(info, len, &dg);
    if (rc != XMSG_OK)
    {
        n->stats.bad_headers++;
        XMSG_LOG(XMSG_LOG_NODE, XMSG_LOG_WARN,
                 "datagram of %u bytes rejected (%d): %02X %02X %02X %02X", (unsigned)len, rc,
                 len > 0 ? info[0] : 0, len > 1 ? info[1] : 0, len > 2 ? info[2] : 0,
                 len > 3 ? info[3] : 0);
        return;
    }
    XMSG_LOG(XMSG_LOG_DG, XMSG_LOG_TRACE, "RX %s from %u F1=0x%04X F2=0x%04X body %u",
             xmsg_dg_subtype_name(dg.hdr.subtype), dg.hdr.src_node, dg.hdr.flags1, dg.hdr.flags2,
             (unsigned)dg.body_len);
    handle_datagram(n, &dg, n->rx_now_ms);
}

/* Every received LAPB frame restarts the settle clock (while not yet ready) and goes to LAPB. */
static void node_rx_frame(XmsgNode *n, const uint8_t *frame, size_t len, uint32_t now_ms)
{
    n->rx_now_ms = now_ms;
    n->quiet_since_ms = now_ms;
    n->quiet_valid = true;
    (void)lapb_rx_frame(&n->lapb, frame, len, now_ms);
}

int xmsg_node_feed(XmsgNode *n, const uint8_t *bytes, size_t len, uint32_t now_ms)
{
    size_t off = 0;
    if (n->cfg.framing != XMSG_FRAMING_ASYNC)
    {
        return XMSG_ESTATE;
    }
    while (off < len)
    {
        size_t used = 0;
        byte_frame_rx_status st = byte_frame_rx_feed(&n->rx, bytes + off, len - off, &used);
        off += used;
        if (st == BYTE_FRAME_RX_FRAME)
        {
            size_t fl;
            const uint8_t *f = byte_frame_rx_frame(&n->rx, &fl);
            node_rx_frame(n, f, fl, now_ms);
            node_pump(n, now_ms);
        }
        else if (st != BYTE_FRAME_RX_NEED_MORE)
        {
            XMSG_LOG(XMSG_LOG_HDLC, XMSG_LOG_DEBUG, "framing status %d", (int)st);
        }
    }
    node_pump(n, now_ms);
    return XMSG_OK;
}

int xmsg_node_feed_frame(XmsgNode *n, const uint8_t *frame, size_t len, uint32_t now_ms)
{
    if (n->cfg.framing != XMSG_FRAMING_RAW)
    {
        return XMSG_ESTATE;
    }
    if (!n->cfg.transport_adds_fcs)
    {
        if (!hdlc_fcs_is_good(frame, len))
        {
            n->rx.stats.fcs_errors++;
            return XMSG_EPROTO;
        }
        len -= HDLC_FCS_SIZE;
    }
    node_rx_frame(n, frame, len, now_ms);
    node_pump(n, now_ms);
    return XMSG_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* Sending                                                                                    */
/* ------------------------------------------------------------------------------------------ */

int xmsg_node_send_begin(XmsgNode *n, uint8_t **body, size_t *cap)
{
    if (!n->ready)
    {
        return XMSG_ESTATE;
    }
    if (n->stage_open || lapb_tx_free_slots(&n->lapb) < 1)
    {
        return XMSG_EAGAIN;
    }
    n->stage_open = true;
    *body = n->stage;
    *cap = XMSG_DG_MAX_BODY;
    return XMSG_OK;
}

void xmsg_node_send_cancel(XmsgNode *n)
{
    n->stage_open = false;
}

/* Put the staged body on the wire at a given Flags1, as one frame or two fragments. */
static int commit_at(XmsgNode *n, uint16_t peer_node, const XmsgSubhdr *sub_in, size_t body_len,
                     uint16_t flags1, uint32_t now_ms)
{
    XmsgSubhdr sub = *sub_in;
    uint8_t *info;
    size_t cap;
    size_t written;
    int rc;

    sub.src_system = n->cfg.own_system;

    if (body_len <= XMSG_DG_FIRST_FRAG_BODY)
    {
        rc = lapb_tx_reserve(&n->lapb, &info, &cap);
        if (rc != XMSG_OK)
        {
            return rc;
        }
        written = xmsg_dg_write_data(info, cap, (uint8_t)SINTRAN_SUBTYPE_DATA, peer_node,
                                     n->cfg.own_node, flags1, sub.xmcsm, &sub, n->stage, body_len);
        if (written == 0)
        {
            lapb_tx_cancel(&n->lapb);
            return XMSG_EINVAL;
        }
        rc = lapb_tx_commit(&n->lapb, written, now_ms);
        if (rc != XMSG_OK)
        {
            return rc;
        }
        n->stats.datagrams_out++;
        return XMSG_OK;
    }

    /* Two fragments need two slots; take none unless both are free. */
    if (lapb_tx_free_slots(&n->lapb) < 2)
    {
        return XMSG_EAGAIN;
    }
    rc = lapb_tx_reserve(&n->lapb, &info, &cap);
    if (rc != XMSG_OK)
    {
        return rc;
    }
    /* The first fragment declares the TOTAL length in Flags2 and XMCSM. */
    sub.xmcsm = (uint16_t)body_len;
    written = xmsg_dg_write_data(info, cap, (uint8_t)SINTRAN_SUBTYPE_FIRST_FRAG, peer_node,
                                 n->cfg.own_node, flags1, (uint16_t)body_len, &sub, n->stage,
                                 XMSG_DG_FIRST_FRAG_BODY);
    if (written == 0)
    {
        lapb_tx_cancel(&n->lapb);
        return XMSG_EINVAL;
    }
    rc = lapb_tx_commit(&n->lapb, written, now_ms);
    if (rc != XMSG_OK)
    {
        return rc;
    }
    rc = lapb_tx_reserve(&n->lapb, &info, &cap);
    if (rc != XMSG_OK)
    {
        return rc; /* cannot happen: two slots were free a moment ago */
    }
    written = xmsg_dg_write_continuation(
        info, cap, peer_node, n->cfg.own_node, flags1, (uint16_t)XMSG_DG_FIRST_FRAG_BODY,
        n->stage + XMSG_DG_FIRST_FRAG_BODY, body_len - XMSG_DG_FIRST_FRAG_BODY);
    if (written == 0)
    {
        lapb_tx_cancel(&n->lapb);
        return XMSG_EINVAL;
    }
    rc = lapb_tx_commit(&n->lapb, written, now_ms);
    if (rc != XMSG_OK)
    {
        return rc;
    }
    n->stats.datagrams_out++;
    return XMSG_OK;
}

int xmsg_node_send_commit(XmsgNode *n, uint16_t peer_node, const XmsgSubhdr *sub, size_t body_len,
                          uint32_t now_ms, uint16_t *flags1_used)
{
    uint16_t flags1;
    int rc;

    if (!n->stage_open)
    {
        return XMSG_ESTATE;
    }
    if (sub == NULL || body_len == 0 || body_len > XMSG_DG_MAX_BODY)
    {
        return XMSG_EINVAL;
    }
    /* Make sure the slots exist BEFORE spending a number: a number spent on nothing is a gap
     * the peer would wait for. */
    if (lapb_tx_free_slots(&n->lapb) < ((body_len > XMSG_DG_FIRST_FRAG_BODY) ? 2 : 1))
    {
        return XMSG_EAGAIN;
    }
    rc = xmsg_flags1_allocate(&n->flags1, peer_node, &flags1);
    if (rc != XMSG_OK)
    {
        return rc;
    }
    rc = commit_at(n, peer_node, sub, body_len, flags1, now_ms);
    if (rc != XMSG_OK)
    {
        return rc;
    }
    n->stage_open = false;
    if (flags1_used != NULL)
    {
        *flags1_used = flags1;
    }
    XMSG_LOG(XMSG_LOG_DG, XMSG_LOG_TRACE, "TX DATA to %u F1=0x%04X body %u", peer_node, flags1,
             (unsigned)body_len);
    node_pump(n, now_ms);
    return XMSG_OK;
}

int xmsg_node_send_repeat(XmsgNode *n, uint16_t peer_node, const XmsgSubhdr *sub, size_t body_len,
                          uint16_t flags1, uint32_t now_ms)
{
    int rc;
    if (!n->stage_open)
    {
        return XMSG_ESTATE;
    }
    if (sub == NULL || body_len == 0 || body_len > XMSG_DG_MAX_BODY)
    {
        return XMSG_EINVAL;
    }
    rc = commit_at(n, peer_node, sub, body_len, flags1, now_ms);
    if (rc != XMSG_OK)
    {
        return rc;
    }
    n->stage_open = false;
    XMSG_LOG(XMSG_LOG_DG, XMSG_LOG_TRACE, "TX REPEAT to %u F1=0x%04X body %u", peer_node, flags1,
             (unsigned)body_len);
    node_pump(n, now_ms);
    return XMSG_OK;
}

int xmsg_node_send_reach(XmsgNode *n, uint16_t peer_node, uint32_t now_ms)
{
    if (!n->link_up)
    {
        return XMSG_ESTATE;
    }
    queue_short(n, (uint8_t)SINTRAN_SUBTYPE_REACH_REQUEST, peer_node, (uint16_t)XMSG_FLAGS1_REACH,
                0x0001);
    node_pump(n, now_ms);
    return XMSG_OK;
}

int xmsg_node_resync(XmsgNode *n, uint16_t peer_node, uint32_t now_ms)
{
    if (n == NULL)
    {
        return XMSG_EINVAL;
    }
    if (!n->link_up)
    {
        return XMSG_ESTATE;
    }
    if (n->resync_pending && n->resync_peer == peer_node)
    {
        if ((int32_t)(now_ms - n->resync_sent_ms) < (int32_t)n->cfg.resync_timeout_ms)
        {
            return XMSG_OK; /* still waiting for the reply */
        }
        XMSG_LOG(XMSG_LOG_NODE, XMSG_LOG_WARN,
                 "node %u: no reachability reply in %u ms: asking again", peer_node,
                 (unsigned)n->cfg.resync_timeout_ms);
    }
    else
    {
        XMSG_LOG(XMSG_LOG_NODE, XMSG_LOG_WARN,
                 "node %u drops our frames in silence: asking it to start its count for us again",
                 peer_node);
    }
    n->resync_pending = true;
    n->resync_peer = peer_node;
    n->resync_sent_ms = now_ms;
    return xmsg_node_send_reach(n, peer_node, now_ms);
}

/* ------------------------------------------------------------------------------------------ */
/* The pump                                                                                   */
/* ------------------------------------------------------------------------------------------ */

/* Move short frames and session datagrams into the window while it has room. */
static void fill_window(XmsgNode *n, uint32_t now_ms)
{
    /* Short frames (ACKs, reachability) first: the peer is waiting for them. */
    while (n->pending_count > 0 && lapb_tx_free_slots(&n->lapb) > 0)
    {
        XmsgPendingShort *p = &n->pending_short[n->pending_head];
        uint8_t *info;
        size_t cap;
        if (lapb_tx_reserve(&n->lapb, &info, &cap) != XMSG_OK)
        {
            break;
        }
        (void)xmsg_dg_write_short(info, p->subtype, p->dst_node, n->cfg.own_node, p->flags1,
                                  p->flags2);
        if (lapb_tx_commit(&n->lapb, SINTRAN_HDR_SIZE, now_ms) != XMSG_OK)
        {
            lapb_tx_cancel(&n->lapb);
            break;
        }
        n->pending_head = (uint8_t)((n->pending_head + 1) % XMSG_NODE_PENDING_SHORT);
        n->pending_count--;
    }

    /* Then routing replies: the host's operator command is waiting on them. They are written
     * straight into the window, not through the staging buffer, so an open send does not block
     * them. */
    while (n->ready && n->pending_route_count > 0 && lapb_tx_free_slots(&n->lapb) > 0)
    {
        if (!send_route_reply(n, now_ms))
        {
            break;
        }
    }

    /* Then sessions, round robin, one datagram each per pass while slots remain. */
    if (!n->ready || n->stage_open)
    {
        return;
    }
    int progress = 1;
    while (progress && lapb_tx_free_slots(&n->lapb) > 0)
    {
        progress = 0;
        for (int k = 0; k < XMSG_MAX_SESSIONS && lapb_tx_free_slots(&n->lapb) > 0; k++)
        {
            int i = (n->next_session_poll + k) % XMSG_MAX_SESSIONS;
            XmsgSession *s = n->sessions[i];
            if (s == NULL || !s->wants_tx || s->ops->poll_tx == NULL)
            {
                continue;
            }
            if (s->ops->poll_tx(s, now_ms) > 0)
            {
                progress = 1;
                n->next_session_poll = (uint8_t)((i + 1) % XMSG_MAX_SESSIONS);
            }
            if (n->stage_open)
            {
                return; /* a session left a send open; it will finish it later */
            }
        }
    }
}

/* Push encoded bytes to the transport; keep what it did not take. */
static void drain_transport(XmsgNode *n, uint32_t now_ms)
{
    (void)now_ms;
    for (;;)
    {
        if (n->tx_sent < n->tx_len)
        {
            int w = n->ports.transport.write(n->ports.transport.ctx, n->tx_scratch + n->tx_sent,
                                             n->tx_len - n->tx_sent);
            if (w < 0)
            {
                XMSG_LOG(XMSG_LOG_NODE, XMSG_LOG_ERROR, "transport write failed (%d); link lost",
                         w);
                xmsg_node_transport_lost(n);
                return;
            }
            n->tx_sent += (size_t)w;
            if (n->tx_sent < n->tx_len)
            {
                return; /* the transport is full; tick retries */
            }
        }
        /* Scratch is empty: more of the current frame, or the next frame. */
        n->tx_len = 0;
        n->tx_sent = 0;
        if (n->cfg.framing == XMSG_FRAMING_ASYNC)
        {
            if (!n->tx_frame_open)
            {
                int fl = lapb_pull_tx(&n->lapb, n->tx_frame, sizeof n->tx_frame);
                if (fl <= 0)
                {
                    return;
                }
                byte_frame_tx_begin(&n->tx, n->tx_frame, (size_t)fl);
                n->tx_frame_open = true;
            }
            n->tx_len = byte_frame_tx_produce(&n->tx, n->tx_scratch, sizeof n->tx_scratch);
            if (byte_frame_tx_done(&n->tx))
            {
                n->tx_frame_open = false;
            }
            if (n->tx_len == 0)
            {
                return;
            }
        }
        else
        {
            int fl = lapb_pull_tx(&n->lapb, n->tx_frame, sizeof n->tx_frame);
            if (fl <= 0)
            {
                return;
            }
            memcpy(n->tx_scratch, n->tx_frame, (size_t)fl);
            n->tx_len = (size_t)fl;
            if (!n->cfg.transport_adds_fcs)
            {
                hdlc_fcs_append(n->tx_scratch + fl, hdlc_fcs_compute(n->tx_scratch, (size_t)fl));
                n->tx_len += HDLC_FCS_SIZE;
            }
        }
    }
}

/*
 * The pump never re-enters itself. A session's poll_tx commits a datagram, and that commit must
 * not pump, or the same session would be polled again before it has counted the send - found as
 * every message going out several times over (2026-11-08).
 */
static void node_pump(XmsgNode *n, uint32_t now_ms)
{
    if (n->pumping)
    {
        return;
    }
    n->pumping = true;
    fill_window(n, now_ms);
    drain_transport(n, now_ms);
    n->pumping = false;
}

int xmsg_node_tick(XmsgNode *n, uint32_t now_ms)
{
    lapb_tick(&n->lapb, now_ms);

    /* Settle: the link is up and nothing has arrived for settle_ms. */
    if (n->link_up && !n->ready)
    {
        if (!n->quiet_valid)
        {
            n->quiet_since_ms = now_ms;
            n->quiet_valid = true;
        }
        else if ((int32_t)(now_ms - n->quiet_since_ms) >= (int32_t)n->cfg.settle_ms)
        {
            n->ready = true;
            XMSG_LOG(XMSG_LOG_NODE, XMSG_LOG_INFO, "link settled after %u ms quiet: ready",
                     (unsigned)n->cfg.settle_ms);
            notify_link(n, lapb_state(&n->lapb), "settled");
        }
    }

    for (int i = 0; i < XMSG_MAX_SESSIONS; i++)
    {
        XmsgSession *s = n->sessions[i];
        if (s != NULL && s->ops->tick != NULL)
        {
            s->ops->tick(s, now_ms);
        }
    }
    node_pump(n, now_ms);
    return 0;
}
