/*
 * host_tad_telnet.c - telnet to a SINTRAN machine: each telnet client gets its own TAD session.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * The tool dials the HDLC line of a SINTRAN machine (nd100x started with --hdlc=N:PORT listens
 * for it), brings the XMSG link up as node --node, and listens for telnet clients. Every client
 * that connects gets a terminal session on the host, as CONNECT-TO would give it on a real ND
 * machine. Up to XMSG_MAX_SESSIONS clients at a time.
 *
 *   host_tad_telnet --hdlc 127.0.0.1:1362 --node 19999 --peer 100 --system D100 --listen 2300
 *                   [--flags1 flags1.txt] [--log info]
 *
 * The SINTRAN side must know our node: the link is defined there for it, and XMSG is running.
 *
 * Terminal behaviour: every byte is forwarded untouched, both ways.
 *  - telnet: the tool opens with the usual server set - it will echo, suppress go-ahead and
 *    send binary, and asks the client to suppress go-ahead and send binary (RFC 854, 856, 857,
 *    858) - so the client sends each key as typed, 8 bits, with no CR translation. Other options
 *    are refused. Telnet commands are taken out of the stream; IAC IAC is the data byte 0xFF.
 *  - typed bytes (ESC, control keys, backspace, everything) go to the host as they are, in
 *    order, as soon as it grants input, up to 255 bytes per grant. No line editing.
 *  - the tool echoes typed bytes back to the client when the host's ECKM says so (the host
 *    does not echo; ECKM 0xFF, for a password, means no echo).
 *  - host text goes to the client byte for byte (0xFF doubled, as telnet needs).
 *  - keystrokes carry even parity in bit 7, as the real asker sends them (the library's
 *    keystroke_parity setting, measured); see libxmsg/tad/tad_asker.h. --raw-keys turns it
 *    off so each typed byte leaves exactly as typed.
 *
 * The Flags1 count to each peer is kept in a small text file (--flags1, default
 * host_tad_telnet.flags1): "node N" (our own node number, see store_read), one "peer next"
 * line per host, and the port-word seed on a line "seed N", so a restart of the
 * tool continues where it stopped and does not reopen the port words of its last run.
 *
 * This is a host program, so it may print, allocate and use sockets; the library does none of
 * those.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET Sock;
#define SOCK_NONE INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int Sock;
#define SOCK_NONE (-1)
#endif

#include "libxmsg/xmsg.h"

/* ------------------------------------------------------------------------------------------ */
/* Platform                                                                                   */
/* ------------------------------------------------------------------------------------------ */

static uint32_t now_ms(void)
{
#ifdef _WIN32
    return (uint32_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
#endif
}

static void sock_close(Sock s)
{
    if (s == SOCK_NONE)
    {
        return;
    }
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

static void sock_nonblocking(Sock s)
{
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
}

static bool sock_would_block(void)
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

/* Non-blocking send: bytes taken, 0 when the buffer is full, -1 on a dead socket. */
static int sock_send(Sock s, const uint8_t *b, size_t n)
{
    int r = (int)send(s, (const char *)b, (int)n, 0);
    if (r < 0)
    {
        return sock_would_block() ? 0 : -1;
    }
    return r;
}

/* Non-blocking receive: bytes read, 0 when nothing waits, -1 when closed or failed. */
static int sock_recv(Sock s, uint8_t *b, size_t cap)
{
    int r = (int)recv(s, (char *)b, (int)cap, 0);
    if (r == 0)
    {
        return -1;
    }
    if (r < 0)
    {
        return sock_would_block() ? 0 : -1;
    }
    return r;
}

static Sock sock_dial(const char *host, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    Sock s = SOCK_NONE;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0)
    {
        return SOCK_NONE;
    }
    for (struct addrinfo *a = res; a != NULL; a = a->ai_next)
    {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == SOCK_NONE)
        {
            continue;
        }
        if (connect(s, a->ai_addr, (int)a->ai_addrlen) == 0)
        {
            break;
        }
        sock_close(s);
        s = SOCK_NONE;
    }
    freeaddrinfo(res);
    if (s != SOCK_NONE)
    {
        sock_nonblocking(s);
    }
    return s;
}

static Sock sock_listen(uint16_t port)
{
    struct sockaddr_in sa;
    int one = 1;
    Sock s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == SOCK_NONE)
    {
        return SOCK_NONE;
    }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(s, 4) != 0)
    {
        sock_close(s);
        return SOCK_NONE;
    }
    sock_nonblocking(s);
    return s;
}

/* ------------------------------------------------------------------------------------------ */
/* Flags1 kept in a text file: one line per peer, "peer next"                                 */
/* ------------------------------------------------------------------------------------------ */

#define STORE_PEERS 8

typedef struct FileStore
{
    const char *path;
    uint16_t own; /* our node number: the line "node N" */
    uint16_t peer[STORE_PEERS];
    uint16_t next[STORE_PEERS];
    int count;
    uint16_t seed; /* the port-word generator, line "seed N" */
    bool has_seed;
} FileStore;

/*
 * The first line, "node N", names OUR node number. The counts are a host's view of US, so a file
 * written while we were another node number must not be used: on 2026-11-09 a count kept for
 * node 19999 was used for node 101 and D100 then dropped node 101's frames in silence. A file
 * with another node number is ignored (with a message); a file with no "node" line is taken
 * as ours. */
static void store_read(FileStore *st)
{
    FILE *f = fopen(st->path, "r");
    char word[16];
    unsigned v;
    st->count = 0;
    st->has_seed = false;
    if (f == NULL)
    {
        return;
    }
    while (fscanf(f, "%15s %u", word, &v) == 2)
    {
        if (strcmp(word, "node") == 0)
        {
            if (v != st->own)
            {
                printf("%s was written for node %u, we are node %u: its counts are not used\n",
                       st->path, v, (unsigned)st->own);
                st->count = 0;
                st->has_seed = false;
                break;
            }
        }
        else if (strcmp(word, "seed") == 0)
        {
            st->seed = (uint16_t)v;
            st->has_seed = true;
        }
        else if (st->count < STORE_PEERS)
        {
            st->peer[st->count] = (uint16_t)strtoul(word, NULL, 10);
            st->next[st->count] = (uint16_t)v;
            st->count++;
        }
    }
    fclose(f);
}

static int store_load(void *ctx, uint16_t peer, uint16_t *next)
{
    FileStore *st = (FileStore *)ctx;
    for (int i = 0; i < st->count; i++)
    {
        if (st->peer[i] == peer)
        {
            *next = st->next[i];
            return 0;
        }
    }
    return -1;
}

static int store_write(const FileStore *st)
{
    FILE *f = fopen(st->path, "w");
    if (f == NULL)
    {
        return -1;
    }
    fprintf(f, "node %u\n", (unsigned)st->own);
    for (int i = 0; i < st->count; i++)
    {
        fprintf(f, "%u %u\n", st->peer[i], st->next[i]);
    }
    if (st->has_seed)
    {
        fprintf(f, "seed %u\n", st->seed);
    }
    return (fclose(f) == 0) ? 0 : -1;
}

static int store_save(void *ctx, uint16_t peer, uint16_t next)
{
    FileStore *st = (FileStore *)ctx;
    int i;
    for (i = 0; i < st->count && st->peer[i] != peer; i++)
    {
    }
    if (i == st->count)
    {
        if (st->count >= STORE_PEERS)
        {
            return -1;
        }
        st->count++;
        st->peer[i] = peer;
    }
    st->next[i] = next;
    return store_write(st);
}

static int store_load_seed(void *ctx, uint16_t *seed)
{
    const FileStore *st = (const FileStore *)ctx;
    if (!st->has_seed)
    {
        return -1;
    }
    *seed = st->seed;
    return 0;
}

static int store_save_seed(void *ctx, uint16_t seed)
{
    FileStore *st = (FileStore *)ctx;
    st->seed = seed;
    st->has_seed = true;
    return store_write(st);
}

/* ------------------------------------------------------------------------------------------ */
/* Telnet clients                                                                             */
/* ------------------------------------------------------------------------------------------ */

#define TN_IAC    0xFFu
#define TN_DONT   0xFEu
#define TN_DO     0xFDu
#define TN_WONT   0xFCu
#define TN_WILL   0xFBu
#define TN_SB     0xFAu
#define TN_SE     0xF0u
#define TN_BINARY 0x00u /* RFC 856: 8-bit data, no CR translation */
#define TN_ECHO   0x01u /* RFC 857: we echo, the client does not */
#define TN_SGA    0x03u /* RFC 858: no go-ahead, i.e. character at a time */

typedef enum TelnetRx
{
    TN_DATA = 0,
    TN_GOT_IAC,
    TN_GOT_VERB,
    TN_IN_SB,
    TN_IN_SB_IAC
} TelnetRx;

typedef struct Client
{
    bool used;
    Sock sock;
    TadAsker asker;
    bool connect_pending; /* waiting for the link before the letter can go */
    TelnetRx tn;
    uint8_t verb;        /* the WILL/WONT/DO/DONT whose option byte comes next */
    bool us_on[256];     /* options WE do (agreed): ECHO, SGA, BINARY */
    bool him_on[256];    /* options the CLIENT does (agreed): SGA, BINARY */
    bool after_cr;       /* the last data byte was CR (for the NVT CR NUL rule) */
    bool echo;           /* the host's ECKM: echo typed bytes back to the client */
    uint8_t ready[1024]; /* typed bytes, in order, waiting for an input grant */
    size_t ready_len;
    uint8_t out[16384]; /* bytes for the client socket */
    size_t out_len;
    bool closing; /* flush and close */
    int index;
} Client;

static Client s_clients[XMSG_MAX_SESSIONS];
static XmsgNode s_node;
static uint16_t s_peer;
static const char *s_system;
static bool s_raw_keys; /* --raw-keys: typed bytes leave without the parity bit */

static void client_put(Client *c, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if (c->out_len + 2 > sizeof c->out)
        {
            return; /* the client is not reading; drop rather than block the line */
        }
        if (b[i] == TN_IAC)
        {
            c->out[c->out_len++] = TN_IAC;
        }
        c->out[c->out_len++] = b[i];
    }
}

static void client_say(Client *c, const char *fmt, ...)
{
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (n > 0)
    {
        client_put(c, (const uint8_t *)text,
                   (size_t)((n < (int)sizeof text) ? n : (int)sizeof text - 1));
    }
}

static void cb_text(void *ctx, const uint8_t *bytes, size_t len)
{
    client_put((Client *)ctx, bytes, len);
}

static void cb_state(void *ctx, TadAskerState st, int error)
{
    Client *c = (Client *)ctx;
    printf("client %d: session %s", c->index, tad_asker_state_name(st));
    if (st == TAD_ASKER_FAILED)
    {
        printf(" (error %d%s%s)", error, (error > 0) ? ": " : "",
               (error > 0) ? xrout_error_name((uint8_t)error) : "");
    }
    printf("\n");
    if (st == TAD_ASKER_CLOSED || st == TAD_ASKER_FAILED)
    {
        client_say(c, "\r\n[session %s]\r\n", tad_asker_state_name(st));
        c->closing = true;
    }
}

static void cb_echo(void *ctx, uint8_t eckm)
{
    ((Client *)ctx)->echo = (eckm == 0x01);
}

static void client_start(Client *c, uint32_t now)
{
    TadAskerConfig cfg;
    TadAskerCallbacks cb = {c, cb_text, cb_state, NULL, cb_echo, NULL};
    tad_asker_config_default(&cfg, s_peer, s_system);
    cfg.keystroke_parity = !s_raw_keys;
    /* What TADA on the host shows for this session (7IAM): terminal = client slot + 1. */
    cfg.terminal_number = (uint16_t)((c - s_clients) + 1);
    cfg.user_name = "TELNET";
    if (tad_asker_init(&c->asker, &cfg, &cb) != XMSG_OK)
    {
        client_say(c, "bad system name\r\n");
        c->closing = true;
        return;
    }
    c->connect_pending = true;
    (void)now;
}

/* One data byte from the client, as typed: queued untouched, in order, for the host. The only
 * rule applied is telnet's own: outside binary mode a CR is sent by the client as CR NUL, and
 * that NUL is not a typed byte (RFC 854). Nothing else is edited, dropped or held for Return. */
static void client_byte(Client *c, uint8_t k)
{
    if (c->after_cr && k == 0x00 && !c->him_on[TN_BINARY])
    {
        c->after_cr = false;
        return;
    }
    c->after_cr = (k == '\r');
    if (c->ready_len >= sizeof c->ready)
    {
        return; /* the host is not taking input; the client typed 1 KB ahead of it */
    }
    c->ready[c->ready_len++] = k;
    if (c->echo)
    {
        client_put(c, &k, 1);
    }
}

static void tn_send(Client *c, uint8_t verb, uint8_t opt)
{
    if (c->out_len + 3 <= sizeof c->out)
    {
        c->out[c->out_len++] = TN_IAC;
        c->out[c->out_len++] = verb;
        c->out[c->out_len++] = opt;
    }
}

/* The options we do ourselves, and the ones we let the client do. */
static bool tn_we_do(uint8_t opt)
{
    return opt == TN_ECHO || opt == TN_SGA || opt == TN_BINARY;
}

static bool tn_client_may(uint8_t opt)
{
    return opt == TN_SGA || opt == TN_BINARY;
}

/* Option negotiation (RFC 854, with the loop rule of RFC 1143: answer only a CHANGE). */
static void tn_option(Client *c, uint8_t verb, uint8_t opt)
{
    switch (verb)
    {
    case TN_DO:
        if (!tn_we_do(opt))
        {
            tn_send(c, TN_WONT, opt);
        }
        else if (!c->us_on[opt])
        {
            c->us_on[opt] = true;
            tn_send(c, TN_WILL, opt);
        }
        break;
    case TN_DONT:
        if (c->us_on[opt])
        {
            c->us_on[opt] = false;
            tn_send(c, TN_WONT, opt);
        }
        break;
    case TN_WILL:
        if (!tn_client_may(opt))
        {
            tn_send(c, TN_DONT, opt);
        }
        else if (!c->him_on[opt])
        {
            c->him_on[opt] = true;
            tn_send(c, TN_DO, opt);
        }
        break;
    default: /* TN_WONT */
        if (c->him_on[opt])
        {
            c->him_on[opt] = false;
            tn_send(c, TN_DONT, opt);
        }
        break;
    }
}

/* The server's opening: we will echo, suppress go-ahead and send binary; we ask the client to
 * suppress go-ahead and send binary. An answer that merely agrees changes nothing. */
static void tn_open(Client *c)
{
    static const uint8_t mine[] = {TN_ECHO, TN_SGA, TN_BINARY};
    static const uint8_t theirs[] = {TN_SGA, TN_BINARY};
    for (size_t i = 0; i < sizeof mine; i++)
    {
        c->us_on[mine[i]] = true;
        tn_send(c, TN_WILL, mine[i]);
    }
    for (size_t i = 0; i < sizeof theirs; i++)
    {
        c->him_on[theirs[i]] = true;
        tn_send(c, TN_DO, theirs[i]);
    }
}

/* Take telnet commands out; hand every data byte on as it is. */
static void client_rx(Client *c, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        uint8_t x = b[i];
        switch (c->tn)
        {
        case TN_DATA:
            if (x == TN_IAC)
            {
                c->tn = TN_GOT_IAC;
            }
            else
            {
                client_byte(c, x);
            }
            break;
        case TN_GOT_IAC:
            if (x == TN_IAC)
            {
                client_byte(c, x); /* IAC IAC is the data byte 0xFF */
                c->tn = TN_DATA;
            }
            else if (x == TN_SB)
            {
                c->tn = TN_IN_SB;
            }
            else if (x >= TN_WILL)
            {
                c->verb = x; /* WILL, WONT, DO, DONT: one option byte follows */
                c->tn = TN_GOT_VERB;
            }
            else
            {
                c->tn = TN_DATA;
            }
            break;
        case TN_GOT_VERB:
            tn_option(c, c->verb, x);
            c->tn = TN_DATA;
            break;
        case TN_IN_SB:
            c->tn = (x == TN_IAC) ? TN_IN_SB_IAC : TN_IN_SB;
            break;
        default: /* TN_IN_SB_IAC */
            c->tn = (x == TN_SE) ? TN_DATA : TN_IN_SB;
            break;
        }
    }
}

/* Send what has been typed, as it is, as soon as the host lets us type: up to TAD_MAX_DATA bytes
 * per input grant (the host's limit), the rest with the next grant. */
static void client_feed_host(Client *c, uint32_t now)
{
    size_t n = c->ready_len;
    if (n == 0 || !tad_asker_may_type(&c->asker))
    {
        return;
    }
    if (n > TAD_MAX_DATA)
    {
        n = TAD_MAX_DATA;
    }
    int took = tad_asker_write(&c->asker, c->ready, n, now);
    if (took > 0)
    {
        memmove(c->ready, c->ready + took, c->ready_len - (size_t)took);
        c->ready_len -= (size_t)took;
    }
}

static void client_drop(Client *c, uint32_t now)
{
    printf("client %d: gone\n", c->index);
    /* Hang up as a real asker does when its user is done: DCON to the session port. Before
     * there is a session port there is nothing to hang up, so drop it locally. */
    if (tad_asker_hangup(&c->asker, now) != XMSG_OK)
    {
        tad_asker_abort(&c->asker);
    }
    sock_close(c->sock);
    c->used = false;
}

/* ------------------------------------------------------------------------------------------ */
/* The node                                                                                   */
/* ------------------------------------------------------------------------------------------ */

static Sock s_line = SOCK_NONE;

static int line_write(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    if (s_line == SOCK_NONE)
    {
        return XMSG_ESTATE;
    }
    int r = sock_send(s_line, b, n);
    return (r < 0) ? XMSG_ESTATE : r;
}

static void log_line(void *ctx, XmsgLogCategory cat, XmsgLogLevel lvl, const char *line)
{
    static const char *const names[] = {"ERROR", "WARN", "INFO", "DEBUG", "TRACE"};
    (void)ctx;
    printf("[%u %s] %s\n", (unsigned)cat, names[(unsigned)lvl < 5 ? (unsigned)lvl : 4], line);
}

static void on_link(void *ctx, LapbState st, bool up, bool ready, const char *reason)
{
    (void)ctx;
    printf("link %s up=%d ready=%d (%s)\n", lapb_state_name(st), up ? 1 : 0, ready ? 1 : 0,
           (reason != NULL) ? reason : "");
}

static void usage(void)
{
    printf("usage: host_tad_telnet --hdlc HOST:PORT --node N --peer N --system NAME\n"
           "                       [--raw-keys] [--listen PORT] [--flags1 FILE] [--log "
           "error|warn|info|debug|trace]\n");
}

static bool parse_u16(const char *s, uint16_t *out)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 0 || v > 65535)
    {
        return false;
    }
    *out = (uint16_t)v;
    return true;
}

int main(int argc, char **argv)
{
    const char *hdlc = NULL;
    uint16_t own = 0;
    uint16_t listen_port = 2300;
    XmsgLogLevel level = XMSG_LOG_INFO;
    FileStore store = {"host_tad_telnet.flags1", 0, {0}, {0}, 0, 0, false};
    char host[128];
    char port[16];

    for (int i = 1; i < argc; i++)
    {
        const char *k = argv[i];
        if (strcmp(k, "--raw-keys") == 0)
        {
            s_raw_keys = true;
            continue;
        }
        if (i + 1 >= argc)
        {
            usage();
            return 1;
        }
        i++;
        const char *v = argv[i];
        bool ok = true;
        if (strcmp(k, "--hdlc") == 0)
        {
            hdlc = v;
        }
        else if (strcmp(k, "--node") == 0)
        {
            ok = parse_u16(v, &own);
        }
        else if (strcmp(k, "--peer") == 0)
        {
            ok = parse_u16(v, &s_peer);
        }
        else if (strcmp(k, "--system") == 0)
        {
            s_system = v;
        }
        else if (strcmp(k, "--listen") == 0)
        {
            ok = parse_u16(v, &listen_port);
        }
        else if (strcmp(k, "--flags1") == 0)
        {
            store.path = v;
        }
        else if (strcmp(k, "--log") == 0)
        {
            static const char *const lv[] = {"error", "warn", "info", "debug", "trace"};
            ok = false;
            for (int j = 0; j < 5; j++)
            {
                if (strcmp(v, lv[j]) == 0)
                {
                    level = (XmsgLogLevel)j;
                    ok = true;
                }
            }
        }
        else
        {
            ok = false;
        }
        if (!ok)
        {
            usage();
            return 1;
        }
    }
    const char *colon = (hdlc != NULL) ? strrchr(hdlc, ':') : NULL;
    if (colon == NULL || own == 0 || s_peer == 0 || s_system == NULL ||
        (size_t)(colon - hdlc) >= sizeof host || strlen(colon + 1) >= sizeof port)
    {
        usage();
        return 1;
    }
    memcpy(host, hdlc, (size_t)(colon - hdlc));
    host[colon - hdlc] = '\0';
    snprintf(port, sizeof port, "%s", colon + 1);

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        printf("Winsock did not start\n");
        return 1;
    }
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    store.own = own;
    store_read(&store);

    XmsgNodeConfig cfg;
    XmsgPorts ports;
    XmsgNodeCallbacks cb;
    xmsg_node_config_default(&cfg, own);
    memset(&ports, 0, sizeof ports);
    ports.transport.write = line_write;
    ports.flags1.ctx = &store;
    ports.flags1.load = store_load;
    ports.flags1.save = store_save;
    ports.flags1.load_port_seed = store_load_seed;
    ports.flags1.save_port_seed = store_save_seed;
    ports.log.write = log_line;
    memset(&cb, 0, sizeof cb);
    cb.link_state = on_link;
    xmsg_log_set_all_levels(level);
    if (xmsg_node_init(&s_node, &cfg, &ports, &cb) != XMSG_OK)
    {
        printf("node set-up failed\n");
        return 1;
    }

    Sock lsock = sock_listen(listen_port);
    if (lsock == SOCK_NONE)
    {
        printf("cannot listen on port %u\n", listen_port);
        return 1;
    }
    printf("telnet to port %u; line %s:%s, we are node %u, host node %u (%s)\n", listen_port, host,
           port, own, s_peer, s_system);

    uint32_t next_dial = 0;
    for (;;)
    {
        uint32_t now = now_ms();
        uint8_t buf[2048];

        /* The line: dial, and dial again after a loss. */
        if (s_line == SOCK_NONE && (int32_t)(now - next_dial) >= 0)
        {
            s_line = sock_dial(host, port);
            next_dial = now + 2000;
            if (s_line != SOCK_NONE)
            {
                printf("line connected\n");
                xmsg_node_link_start(&s_node, now);
            }
        }

        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(lsock, &rd);
        Sock maxs = lsock;
        if (s_line != SOCK_NONE)
        {
            FD_SET(s_line, &rd);
            maxs = (s_line > maxs) ? s_line : maxs;
        }
        for (int i = 0; i < XMSG_MAX_SESSIONS; i++)
        {
            if (s_clients[i].used)
            {
                FD_SET(s_clients[i].sock, &rd);
                maxs = (s_clients[i].sock > maxs) ? s_clients[i].sock : maxs;
            }
        }
        struct timeval tv = {0, 10000};
        (void)select((int)maxs + 1, &rd, NULL, NULL, &tv);
        now = now_ms();

        if (s_line != SOCK_NONE && FD_ISSET(s_line, &rd))
        {
            int n = sock_recv(s_line, buf, sizeof buf);
            if (n > 0)
            {
                xmsg_node_feed(&s_node, buf, (size_t)n, now);
            }
            else if (n < 0)
            {
                printf("line lost\n");
                sock_close(s_line);
                s_line = SOCK_NONE;
                xmsg_node_transport_lost(&s_node);
                for (int i = 0; i < XMSG_MAX_SESSIONS; i++)
                {
                    if (s_clients[i].used)
                    {
                        client_say(&s_clients[i], "\r\n[line lost]\r\n");
                        tad_asker_abort(&s_clients[i].asker);
                        s_clients[i].closing = true;
                    }
                }
            }
        }

        if (FD_ISSET(lsock, &rd))
        {
            Sock cs = accept(lsock, NULL, NULL);
            if (cs != SOCK_NONE)
            {
                int i;
                /* A slot is free once its client is gone AND its session has let go of the
                 * node: after a hang-up the DCON is still in flight on that session. */
                for (i = 0; i < XMSG_MAX_SESSIONS &&
                            (s_clients[i].used || s_clients[i].asker.base.attached);
                     i++)
                {
                }
                if (i == XMSG_MAX_SESSIONS)
                {
                    static const char full[] = "all sessions in use\r\n";
                    (void)send(cs, full, (int)sizeof full - 1, 0);
                    sock_close(cs);
                }
                else
                {
                    Client *c = &s_clients[i];
                    memset(c, 0, sizeof *c);
                    c->used = true;
                    c->sock = cs;
                    c->index = i;
                    c->echo = true;
                    sock_nonblocking(cs);
                    tn_open(c);
                    printf("client %d: connected\n", i);
                    client_start(c, now);
                }
            }
        }

        for (int i = 0; i < XMSG_MAX_SESSIONS; i++)
        {
            Client *c = &s_clients[i];
            if (!c->used)
            {
                continue;
            }
            if (FD_ISSET(c->sock, &rd))
            {
                int n = sock_recv(c->sock, buf, sizeof buf);
                if (n < 0)
                {
                    client_drop(c, now);
                    continue;
                }
                if (!c->closing)
                {
                    client_rx(c, buf, (size_t)n);
                }
            }
            if (c->connect_pending && xmsg_node_is_ready(&s_node))
            {
                int rc = tad_asker_connect(&c->asker, &s_node, now);
                if (rc == XMSG_OK)
                {
                    c->connect_pending = false;
                }
                else if (rc != XMSG_ESTATE)
                {
                    client_say(c, "no free session on the node (%d)\r\n", rc);
                    c->connect_pending = false;
                    c->closing = true;
                }
            }
            client_feed_host(c, now);
        }

        xmsg_node_tick(&s_node, now);

        for (int i = 0; i < XMSG_MAX_SESSIONS; i++)
        {
            Client *c = &s_clients[i];
            if (!c->used)
            {
                continue;
            }
            if (c->out_len > 0)
            {
                int n = sock_send(c->sock, c->out, c->out_len);
                if (n < 0)
                {
                    client_drop(c, now);
                    continue;
                }
                memmove(c->out, c->out + n, c->out_len - (size_t)n);
                c->out_len -= (size_t)n;
            }
            if (c->closing && c->out_len == 0)
            {
                client_drop(c, now);
            }
        }
    }
}
