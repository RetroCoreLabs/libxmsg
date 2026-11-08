/*
 * test_pair.h - two test endpoints that talk to each other: in memory, or over loopback TCP.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Every layer of the library gets a peer-to-peer test: two framers, two LAPB stations, two nodes,
 * a client against a server. They need a "wire" between them, and this is it, in two shapes:
 *
 *   TestPair     in-memory, cross-connected byte queues. Deterministic, runs in one process, and
 *                each direction can inject faults: drop a write, duplicate it, flip a bit in it,
 *                or split it into small chunks (which is what a real TCP read does). Hold-and-
 *                release lets a test control exactly when bytes arrive.
 *
 *   TestTcpPair  a real socket pair over 127.0.0.1 (Winsock on Windows, BSD sockets elsewhere),
 *                non-blocking, so the same code can also run as two separate processes with
 *                test_tcp_listen() / test_tcp_connect(). This is the portable "pipe".
 *
 * Both expose an XmsgTransportPort for a side, so a node or a framer can be wired to them without
 * glue in the test. Test code may use malloc and stdio; the library may not.
 */
#ifndef TEST_PAIR_H
#define TEST_PAIR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libxmsg/xmsg_ports.h"

/* ------------------------------------------------------------------------------------------ */
/* In-memory pair                                                                             */
/* ------------------------------------------------------------------------------------------ */

/** Capacity of each direction's queue. A LAPB window of 7 x 626 bytes fits many times over. */
#define TEST_PAIR_QUEUE_SIZE 65536

/** Faults applied to one direction. Zero means "never". Counts are per write call. */
typedef struct TestFaults
{
    unsigned drop_every;    /**< drop every n-th write entirely */
    unsigned dup_every;     /**< deliver every n-th write twice */
    unsigned corrupt_every; /**< flip one bit in every n-th write */
    size_t max_chunk;       /**< reads return at most this many bytes (0 = no limit) */
    bool hold;              /**< queue writes until test_pair_release() */
} TestFaults;

/** One direction: a byte ring plus its fault policy and counters. */
typedef struct TestChannel
{
    uint8_t ring[TEST_PAIR_QUEUE_SIZE];
    size_t head; /**< next byte to read */
    size_t tail; /**< next byte to write */
    size_t held; /**< bytes written while hold was on, not yet readable */
    TestFaults faults;
    unsigned writes;    /**< write calls seen */
    unsigned dropped;   /**< writes dropped by the fault policy */
    unsigned dupped;    /**< writes duplicated */
    unsigned corrupted; /**< writes with a flipped bit */
    size_t bytes;       /**< bytes delivered into the ring */
} TestChannel;

/** The pair. Side 0 writes into channel[0], which side 1 reads; and the other way round. */
typedef struct TestPair
{
    TestChannel channel[2];
} TestPair;

/** The context behind an XmsgTransportPort for one side of a pair. */
typedef struct TestPairPort
{
    TestPair *pair;
    int side;
} TestPairPort;

/**
 * @brief Set up a pair with no faults.
 * @param p The pair (large: keep it static or heap allocated, not on a small stack).
 */
void test_pair_init(TestPair *p);

/**
 * @brief Set the fault policy for what `from_side` writes.
 * @param p         Pair.
 * @param from_side 0 or 1.
 * @param faults    Copied; NULL clears every fault.
 */
void test_pair_set_faults(TestPair *p, int from_side, const TestFaults *faults);

/**
 * @brief Write bytes from one side; they become readable on the other, subject to faults.
 * @return Bytes accepted (all of them, or 0 when the queue is full).
 */
int test_pair_write(TestPair *p, int from_side, const uint8_t *bytes, size_t len);

/**
 * @brief Read bytes that arrived at one side.
 * @return Bytes copied, 0 when nothing is waiting.
 */
size_t test_pair_read(TestPair *p, int side, uint8_t *out, size_t cap);

/** @brief Bytes waiting to be read at a side (held bytes not counted). */
size_t test_pair_pending(const TestPair *p, int side);

/**
 * @brief Release bytes that were written while `hold` was on, making them readable.
 * @param from_side The writer whose held bytes are released.
 * @param count     How many bytes to release, or 0 for all.
 */
void test_pair_release(TestPair *p, int from_side, size_t count);

/**
 * @brief Fill an XmsgTransportPort that writes into the pair from one side.
 * @param ctx  Storage for the port context; must outlive the port.
 * @param out  Receives the port.
 */
void test_pair_transport(TestPair *p, int side, TestPairPort *ctx, XmsgTransportPort *out);

/* ------------------------------------------------------------------------------------------ */
/* Loopback TCP pair                                                                          */
/* ------------------------------------------------------------------------------------------ */

/** A platform socket handle, large enough for both Winsock and POSIX. -1 when closed. */
typedef intptr_t TestSocket;

/** A connected, non-blocking socket pair over 127.0.0.1. */
typedef struct TestTcpPair
{
    TestSocket sock[2]; /**< side 0 is the connecting end, side 1 the accepted end */
    uint16_t port;      /**< the port the listener used */
} TestTcpPair;

/** Must be called once before any socket use (Winsock start-up); harmless elsewhere. */
int test_tcp_startup(void);

/**
 * @brief Open a connected loopback pair on an ephemeral port.
 * @return 0, or -1 with the reason printed.
 */
int test_tcp_pair_open(TestTcpPair *t);

/** @brief Close both ends. */
void test_tcp_pair_close(TestTcpPair *t);

/** @brief Non-blocking write. Returns bytes accepted, 0 when the socket buffer is full, -1 on error. */
int test_tcp_write(TestSocket s, const uint8_t *bytes, size_t len);

/** @brief Non-blocking read. Returns bytes read, 0 when nothing waits, -1 on error or close. */
int test_tcp_read(TestSocket s, uint8_t *out, size_t cap);

/** @brief Wait up to `ms` milliseconds for the socket to become readable. Returns true if it did. */
bool test_tcp_wait_readable(TestSocket s, int ms);

/**
 * @brief Listen on 127.0.0.1 and accept exactly one connection (blocking, with a timeout).
 * @param port  0 for an ephemeral port; receives the port actually used.
 * @param ms    Milliseconds to wait for the peer.
 * @return The accepted socket, or -1.
 */
TestSocket test_tcp_listen_accept_one(uint16_t *port, int ms);

/**
 * @brief Connect to 127.0.0.1:port. Returns the socket, or -1.
 */
TestSocket test_tcp_connect(uint16_t port);

/**
 * @brief Connect to a named host (name or dotted address) and port, for the integration tests
 *        that reach an nd100x anywhere on the network. Returns the socket, or -1.
 */
TestSocket test_tcp_connect_host(const char *host, uint16_t port);

/** @brief Close one socket. */
void test_tcp_close(TestSocket s);

/** The context behind an XmsgTransportPort writing to a TCP socket. */
typedef struct TestTcpPort
{
    TestSocket sock;
} TestTcpPort;

/** @brief Fill an XmsgTransportPort that writes to a socket. */
void test_tcp_transport(TestSocket s, TestTcpPort *ctx, XmsgTransportPort *out);

/** @brief Sleep for a few milliseconds (so a test can wait for the other process or the stack). */
void test_sleep_ms(int ms);

/** @brief A monotonic millisecond clock for tests that drive real sockets. */
uint32_t test_now_ms(void);

#endif /* TEST_PAIR_H */
