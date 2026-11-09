/*
 * xmsg_ports.h - the services an application supplies to libxmsg.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * The library has no sockets, no serial port, no file system and no clock of its own. Whatever
 * runs it - a PC test tool, an ESP32 sketch, a Pico program - fills in these small tables of
 * function pointers once at start-up. A NULL table or NULL function means "not provided" and the
 * library degrades as each field documents.
 *
 * Why tables and not link-time symbols: one program may run two nodes on two different transports
 * (a TCP bridge to an emulator and a serial gateway to a real machine) and a unit test wants to be
 * the transport itself.
 */
#ifndef XMSG_PORTS_H
#define XMSG_PORTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Where outgoing bytes (or whole frames) go.
 *
 * `write` must not block. It returns how many bytes it accepted, 0 when its buffer is full, or a
 * negative value when the transport is gone. The node keeps the rest and retries from its tick.
 * With async framing the bytes are a flag-delimited HDLC stream; with raw framing each call is one
 * complete frame (address .. info, with or without FCS as configured).
 */
typedef struct XmsgTransportPort
{
    void *ctx;                                                 /**< passed back to write */
    int (*write)(void *ctx, const uint8_t *bytes, size_t len); /**< bytes accepted, 0 or negative */
    /** The line speed in bits per second, for a transport that knows it (a serial or PIO line
     *  on an ESP32 or Pico). NULL, or a return of 0, means not known: a TCP stream has none. */
    uint32_t (*line_speed)(void *ctx);
} XmsgTransportPort;

/**
 * @brief Where the outgoing message counter per peer machine is kept across restarts.
 *
 * SINTRAN expects our datagram sequence to CONTINUE after we restart; a number behind what the
 * machine expects is dropped in silence, a number ahead is refused with an error we can recover
 * from. So the counter is saved through this port - a text file on a PC, a key in NVS on an
 * ESP32, a flash sector on a Pico. Both functions may be NULL: the counters then start at 0 and
 * the node logs one warning.
 *
 * `load` returns 0 and fills `next` when a value exists, -1 when nothing is stored for that peer.
 * `save` returns 0 on success.
 */
typedef struct XmsgFlags1Port
{
    void *ctx;
    int (*load)(void *ctx, uint16_t peer_node, uint16_t *next);
    int (*save)(void *ctx, uint16_t peer_node, uint16_t next);
    /**
     * The seed of the port-word generator, kept by the same store. The random low seven bits of
     * a port word are what stop a reopened port reusing its old identifier, so a restarted node
     * must continue the draws, not repeat them (RetroIO XmsgPortWordAllocator). Without it every
     * run opens the same port word, and a frame the machine still sends to an old session reaches
     * the new one: measured 2026-11-09, an FD for a session of an earlier run ended a fresh login.
     * Both may be NULL: the node then starts from XmsgNodeConfig.portword_seed every time.
     * `load_port_seed` returns 0 and fills `seed` when one is stored, -1 otherwise.
     */
    int (*load_port_seed)(void *ctx, uint16_t *seed);
    int (*save_port_seed)(void *ctx, uint16_t seed);
} XmsgFlags1Port;

/** Log categories, one per subsystem. The minimum level is set per category at run time. */
typedef enum XmsgLogCategory
{
    XMSG_LOG_HDLC = 0,
    XMSG_LOG_LAPB,
    XMSG_LOG_DG,
    XMSG_LOG_FRAG,
    XMSG_LOG_FLAGS1,
    XMSG_LOG_NODE,
    XMSG_LOG_XROUT,
    XMSG_LOG_TAD,
    XMSG_LOG_COSMOS,
    XMSG_LOG_FTP,
    XMSG_LOG_CATEGORY_COUNT
} XmsgLogCategory;

/** Log levels. ERROR is always enabled; the rest are gated per category. */
typedef enum XmsgLogLevel
{
    XMSG_LOG_ERROR = 0,
    XMSG_LOG_WARN = 1,
    XMSG_LOG_INFO = 2,
    XMSG_LOG_DEBUG = 3,
    XMSG_LOG_TRACE = 4
} XmsgLogLevel;

/**
 * @brief Where finished log lines go.
 *
 * The library formats the message (no newline, no timestamp, no category tag); the sink adds
 * whatever its output wants. Guest text - terminal bytes, file contents - never passes through
 * here (house rule 9.5). A NULL sink means silence.
 */
typedef struct XmsgLogPort
{
    void *ctx;
    void (*write)(void *ctx, XmsgLogCategory category, XmsgLogLevel level, const char *line);
} XmsgLogPort;

/** All the ports a node needs, handed to xmsg_node_init() and copied. */
typedef struct XmsgPorts
{
    XmsgTransportPort transport;
    XmsgFlags1Port flags1;
    XmsgLogPort log;
} XmsgPorts;

#ifdef __cplusplus
}
#endif

#endif /* XMSG_PORTS_H */
