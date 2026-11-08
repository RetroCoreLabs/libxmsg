/*
 * test_pair.c - the in-memory pair with fault injection, and the loopback TCP pair.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "pair/test_pair.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#endif

/* ------------------------------------------------------------------------------------------ */
/* In-memory pair                                                                             */
/* ------------------------------------------------------------------------------------------ */

static void channel_init(TestChannel *c)
{
    memset(c, 0, sizeof *c);
}

void test_pair_init(TestPair *p)
{
    channel_init(&p->channel[0]);
    channel_init(&p->channel[1]);
}

void test_pair_set_faults(TestPair *p, int from_side, const TestFaults *faults)
{
    TestChannel *c = &p->channel[from_side & 1];
    if (faults == NULL)
    {
        memset(&c->faults, 0, sizeof c->faults);
    }
    else
    {
        c->faults = *faults;
    }
}

static size_t channel_used(const TestChannel *c)
{
    return (c->tail + TEST_PAIR_QUEUE_SIZE - c->head) % TEST_PAIR_QUEUE_SIZE;
}

static size_t channel_free(const TestChannel *c)
{
    /* One slot is kept empty to tell full from empty. */
    return TEST_PAIR_QUEUE_SIZE - 1 - channel_used(c);
}

static void channel_push(TestChannel *c, const uint8_t *bytes, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        c->ring[c->tail] = bytes[i];
        c->tail = (c->tail + 1) % TEST_PAIR_QUEUE_SIZE;
    }
    c->bytes += len;
}

int test_pair_write(TestPair *p, int from_side, const uint8_t *bytes, size_t len)
{
    TestChannel *c = &p->channel[from_side & 1];
    uint8_t copy[TEST_PAIR_QUEUE_SIZE / 4];
    const uint8_t *data = bytes;
    size_t copies = 1;

    if (len == 0)
    {
        return 0;
    }
    c->writes++;

    if (c->faults.drop_every != 0 && (c->writes % c->faults.drop_every) == 0)
    {
        c->dropped++;
        return (int)len; /* the writer believes it was sent; that is what a lossy line does */
    }
    if (c->faults.corrupt_every != 0 && (c->writes % c->faults.corrupt_every) == 0 &&
        len <= sizeof copy)
    {
        memcpy(copy, bytes, len);
        /* Flip one bit somewhere in the middle, never in a flag position at the very ends, so the
         * fault lands in the frame body or FCS and the decoder must catch it. */
        size_t at = (len > 2) ? 1 + (c->writes % (len - 2)) : 0;
        copy[at] ^= (uint8_t)(1u << (c->writes % 8));
        data = copy;
        c->corrupted++;
    }
    if (c->faults.dup_every != 0 && (c->writes % c->faults.dup_every) == 0)
    {
        copies = 2;
        c->dupped++;
    }

    if (channel_free(c) < len * copies)
    {
        return 0;
    }
    for (size_t k = 0; k < copies; k++)
    {
        channel_push(c, data, len);
    }
    if (c->faults.hold)
    {
        c->held += len * copies;
    }
    return (int)len;
}

size_t test_pair_pending(const TestPair *p, int side)
{
    /* Side `side` reads what the OTHER side wrote. */
    const TestChannel *c = &p->channel[(side & 1) ^ 1];
    return channel_used(c) - c->held;
}

size_t test_pair_read(TestPair *p, int side, uint8_t *out, size_t cap)
{
    TestChannel *c = &p->channel[(side & 1) ^ 1];
    size_t avail = channel_used(c) - c->held;
    size_t n = avail < cap ? avail : cap;

    if (c->faults.max_chunk != 0 && n > c->faults.max_chunk)
    {
        n = c->faults.max_chunk;
    }
    for (size_t i = 0; i < n; i++)
    {
        out[i] = c->ring[c->head];
        c->head = (c->head + 1) % TEST_PAIR_QUEUE_SIZE;
    }
    return n;
}

void test_pair_release(TestPair *p, int from_side, size_t count)
{
    TestChannel *c = &p->channel[from_side & 1];
    if (count == 0 || count > c->held)
    {
        count = c->held;
    }
    c->held -= count;
}

static int pair_port_write(void *ctx, const uint8_t *bytes, size_t len)
{
    TestPairPort *pp = (TestPairPort *)ctx;
    return test_pair_write(pp->pair, pp->side, bytes, len);
}

void test_pair_transport(TestPair *p, int side, TestPairPort *ctx, XmsgTransportPort *out)
{
    ctx->pair = p;
    ctx->side = side;
    out->ctx = ctx;
    out->write = pair_port_write;
}

/* ------------------------------------------------------------------------------------------ */
/* Loopback TCP                                                                               */
/* ------------------------------------------------------------------------------------------ */

#if defined(_WIN32)
typedef SOCKET NativeSocket;
#define NATIVE_INVALID INVALID_SOCKET
static int last_socket_error(void)
{
    return WSAGetLastError();
}
static bool would_block(int err)
{
    return err == WSAEWOULDBLOCK;
}
static void native_close(NativeSocket s)
{
    closesocket(s);
}
static int set_nonblocking(NativeSocket s)
{
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0 ? 0 : -1;
}
#else
typedef int NativeSocket;
#define NATIVE_INVALID (-1)
static int last_socket_error(void)
{
    return errno;
}
static bool would_block(int err)
{
    return err == EAGAIN || err == EWOULDBLOCK;
}
static void native_close(NativeSocket s)
{
    close(s);
}
static int set_nonblocking(NativeSocket s)
{
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0)
    {
        return -1;
    }
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -1;
}
#endif

int test_tcp_startup(void)
{
#if defined(_WIN32)
    static bool started = false;
    if (!started)
    {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        {
            printf("WSAStartup failed\n");
            return -1;
        }
        started = true;
    }
#endif
    return 0;
}

static void disable_nagle(NativeSocket s)
{
    /* Frames are small and the tests wait for them; coalescing would only add latency. */
    int one = 1;
    (void)setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
}

static NativeSocket make_listener(uint16_t *port)
{
    struct sockaddr_in addr;
    socklen_t alen = sizeof addr;
    NativeSocket ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == NATIVE_INVALID)
    {
        printf("socket() failed: %d\n", last_socket_error());
        return NATIVE_INVALID;
    }
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(*port);
    if (bind(ls, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(ls, 1) != 0)
    {
        printf("bind/listen failed: %d\n", last_socket_error());
        native_close(ls);
        return NATIVE_INVALID;
    }
    if (getsockname(ls, (struct sockaddr *)&addr, &alen) == 0)
    {
        *port = ntohs(addr.sin_port);
    }
    return ls;
}

TestSocket test_tcp_connect(uint16_t port)
{
    struct sockaddr_in addr;
    NativeSocket s;

    if (test_tcp_startup() != 0)
    {
        return -1;
    }
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == NATIVE_INVALID)
    {
        return -1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (connect(s, (struct sockaddr *)&addr, sizeof addr) != 0)
    {
        printf("connect(%u) failed: %d\n", (unsigned)port, last_socket_error());
        native_close(s);
        return -1;
    }
    disable_nagle(s);
    if (set_nonblocking(s) != 0)
    {
        native_close(s);
        return -1;
    }
    return (TestSocket)s;
}

static bool native_wait_readable(NativeSocket s, int ms)
{
    fd_set set;
    struct timeval tv;
    FD_ZERO(&set);
    FD_SET(s, &set);
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return select((int)s + 1, &set, NULL, NULL, &tv) > 0;
}

TestSocket test_tcp_listen_accept_one(uint16_t *port, int ms)
{
    NativeSocket ls;
    NativeSocket s;

    if (test_tcp_startup() != 0)
    {
        return -1;
    }
    ls = make_listener(port);
    if (ls == NATIVE_INVALID)
    {
        return -1;
    }
    if (!native_wait_readable(ls, ms))
    {
        printf("no connection within %d ms\n", ms);
        native_close(ls);
        return -1;
    }
    s = accept(ls, NULL, NULL);
    native_close(ls);
    if (s == NATIVE_INVALID)
    {
        return -1;
    }
    disable_nagle(s);
    if (set_nonblocking(s) != 0)
    {
        native_close(s);
        return -1;
    }
    return (TestSocket)s;
}

int test_tcp_pair_open(TestTcpPair *t)
{
    NativeSocket ls;
    NativeSocket client;
    NativeSocket server;
    uint16_t port = 0;

    t->sock[0] = -1;
    t->sock[1] = -1;
    if (test_tcp_startup() != 0)
    {
        return -1;
    }
    ls = make_listener(&port);
    if (ls == NATIVE_INVALID)
    {
        return -1;
    }
    /* Connect first (blocking, loopback: completes at once), then accept. */
    client = (NativeSocket)test_tcp_connect(port);
    if (client == NATIVE_INVALID)
    {
        native_close(ls);
        return -1;
    }
    if (!native_wait_readable(ls, 2000))
    {
        native_close(ls);
        native_close(client);
        return -1;
    }
    server = accept(ls, NULL, NULL);
    native_close(ls);
    if (server == NATIVE_INVALID)
    {
        native_close(client);
        return -1;
    }
    disable_nagle(server);
    if (set_nonblocking(server) != 0)
    {
        native_close(client);
        native_close(server);
        return -1;
    }
    t->sock[0] = (TestSocket)client;
    t->sock[1] = (TestSocket)server;
    t->port = port;
    return 0;
}

void test_tcp_close(TestSocket s)
{
    if (s != -1)
    {
        native_close((NativeSocket)s);
    }
}

void test_tcp_pair_close(TestTcpPair *t)
{
    test_tcp_close(t->sock[0]);
    test_tcp_close(t->sock[1]);
    t->sock[0] = -1;
    t->sock[1] = -1;
}

int test_tcp_write(TestSocket s, const uint8_t *bytes, size_t len)
{
    int n = (int)send((NativeSocket)s, (const char *)bytes, (int)len, 0);
    if (n < 0)
    {
        return would_block(last_socket_error()) ? 0 : -1;
    }
    return n;
}

int test_tcp_read(TestSocket s, uint8_t *out, size_t cap)
{
    int n = (int)recv((NativeSocket)s, (char *)out, (int)cap, 0);
    if (n == 0)
    {
        return -1; /* orderly close */
    }
    if (n < 0)
    {
        return would_block(last_socket_error()) ? 0 : -1;
    }
    return n;
}

bool test_tcp_wait_readable(TestSocket s, int ms)
{
    return native_wait_readable((NativeSocket)s, ms);
}

static int tcp_port_write(void *ctx, const uint8_t *bytes, size_t len)
{
    TestTcpPort *tp = (TestTcpPort *)ctx;
    return test_tcp_write(tp->sock, bytes, len);
}

void test_tcp_transport(TestSocket s, TestTcpPort *ctx, XmsgTransportPort *out)
{
    ctx->sock = s;
    out->ctx = ctx;
    out->write = tcp_port_write;
}

void test_sleep_ms(int ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

uint32_t test_now_ms(void)
{
#if defined(_WIN32)
    return (uint32_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
#endif
}
