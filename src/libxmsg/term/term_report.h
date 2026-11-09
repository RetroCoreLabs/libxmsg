/*
 * term_report.h - the text of the bridge's WHO and INFO commands, from a snapshot.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * The application fills a snapshot struct from its own state and the node; these functions only
 * format it, so they can be tested byte for byte. Lines end with CR LF (telnet text).
 */
#ifndef XMSG_TERM_REPORT_H
#define XMSG_TERM_REPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** One client on the bridge, for WHO. */
typedef struct TermWhoEntry
{
    int terminal;              /**< terminal number (slot + 1) */
    const char *user;          /**< the logged-in bridge user; NULL = still at the login */
    const char *remote;        /**< the client's address, "IP:port" */
    uint32_t connected_s;      /**< seconds since it connected */
    const char *system;        /**< the system it is connected to; NULL = at the prompt */
    const char *session_state; /**< the TAD session state name when connected */
    bool is_self;              /**< the client asking: marked with '*' */
} TermWhoEntry;

/**
 * @brief Format the WHO listing: a heading and one line per client.
 * @param e   Entries.
 * @param n   Their count.
 * @param buf Buffer.
 * @param cap Its size.
 * @return The length written (zero-terminated), or -1 when it does not fit or for NULL.
 */
int term_report_who(const TermWhoEntry *e, size_t n, char *buf, size_t cap);

/** The link facts, for INFO. */
typedef struct TermLinkInfo
{
    uint16_t own_node;         /**< our node number */
    uint16_t own_system;       /**< our XMSG system number */
    int32_t peer_node_learned; /**< from the peer's LAPB frames; -1 = not seen yet */
    uint16_t peer_node_cfg;    /**< the configured peer node */
    const char *peer_system;   /**< the configured system name, e.g. "D100" */
    bool link_up;              /**< LAPB up */
    bool link_ready;           /**< ready for datagrams */
    const char *transport;     /**< e.g. "HDLC over TCP 127.0.0.1:10362" */
    uint32_t line_bps;         /**< line speed when the transport knows it; 0 = not known */
    bool line_is_tcp;          /**< no line speed exists (TCP stream) */
    uint32_t frames_in;
    uint32_t frames_out;
    uint32_t retransmissions;
    uint32_t unusual_address;
    uint8_t peer_window; /**< the peer's receive window from its last address byte */
    uint32_t datagrams_in;
    uint32_t datagrams_out;
    uint32_t uptime_s; /**< seconds since the bridge started */
} TermLinkInfo;

/**
 * @brief Format the INFO text.
 * @param li  The facts.
 * @param buf Buffer.
 * @param cap Its size.
 * @return The length written (zero-terminated), or -1 when it does not fit or for NULL.
 */
int term_report_info(const TermLinkInfo *li, char *buf, size_t cap);

/**
 * @brief Write a duration as "1d 02:03:04" or "02:03:04".
 * @param s   Seconds.
 * @param buf Buffer.
 * @param cap Its size (16 is enough).
 * @return The length, or -1.
 */
int term_report_duration(uint32_t s, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_TERM_REPORT_H */
