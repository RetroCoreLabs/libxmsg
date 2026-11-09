/*
 * host_tad_telnet.c - telnet to a SINTRAN machine: each telnet client gets its own TAD session.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * The tool dials the HDLC line of a SINTRAN machine (nd100x started with --hdlc=N:PORT listens
 * for it), brings the XMSG link up as node --node, and listens for telnet clients. Up to
 * XMSG_MAX_SESSIONS clients at a time.
 *
 *   host_tad_telnet --hdlc 127.0.0.1:1362 --node 19999 --peer 100 --system D100 --listen 2300
 *                   [--mode direct|prompt] [--users FILE] [--esc-ms N] [--term-type N]
 *                   [--user NAME] [--raw-keys] [--flags1 flags1.txt] [--log info]
 *   host_tad_telnet --add-user NAME [--admin] | --del-user NAME | --list-users
 *                   | --reset-password NAME   [--users FILE]
 *
 * The SINTRAN side must know our node: the link is defined there for it, and XMSG is running.
 *
 * Two modes (--mode):
 *  - direct (the default): each telnet client goes straight into a TAD session on --system.
 *    When the SINTRAN login ends (the host's SYCN 000B after LOGOUT), the tool hangs up the TAD
 *    session (DCON) and closes the telnet connection.
 *  - prompt: each telnet client first logs in to the tool ("login: ", "password: "; users from
 *    the --users file), then gets the tool's own command line "TAD> ":
 *      CONNECT <system>   a TAD session on that system (the connect letter names it; the host
 *                         we are linked to routes it on)
 *      LIST               the systems that can be reached (not implemented yet)
 *      WHO                every client on the tool: terminal, user, address, time, what it does
 *      INFO               the link: our node, the peer, link state, line, counters, uptime
 *      PASSWORD           change one's own password (old, new, again)
 *      HELP               the commands
 *      EXIT, LOGOUT, QUIT close the telnet connection
 *    and for admins only (others get "not allowed"):
 *      USERS              the users, with level and whether they must change the password
 *      ADDUSER name [admin]  a new user; asks the password twice; the user must change it at
 *                         the first login
 *      DELUSER name       delete a user (not the last admin)
 *      RESETPW name       set a new password (asked twice); the user must change it at login
 *    When the SINTRAN login ends, the TAD session is hung up and the user is back at "TAD> ".
 *    Three failed logins close the connection; each failure waits one second; the tool never
 *    says whether the name or the password was wrong. A user marked "must change" is asked for
 *    a new password twice right after the login, and the file is written before "TAD> " shows.
 *    When the users file is missing or holds no user, the tool creates it with the default
 *    admin (name admin, password admin, must change) and prints a warning.
 *  - in both modes, when the telnet connection drops, the TAD session is hung up at once, which
 *    logs the user out of SINTRAN.
 *
 * The users file (--users, default host_tad_telnet.users), one line per user, '#' comments:
 *     name:pbkdf2-sha256:<iterations>:<salt hex>:<hash hex>:<flags>
 * flags is "admin" or "user", optionally followed by ",change" (must set a new password at the
 * next login). Lines without the flags field (older files) read as user, no change.
 * PBKDF2-HMAC-SHA256 with a random 16-byte salt per user and 100000 iterations; the password
 * itself is never stored or logged. --add-user NAME [--admin] asks for the password twice with
 * echo off (an existing user is replaced; level user unless --admin), --del-user NAME removes
 * one (not the last admin), --list-users prints the names with level and change flag, and
 * --reset-password NAME asks a new password twice and marks the user "must change".
 * The file is read again at every login, so changes need no restart.
 *
 * Terminal behaviour inside a TAD session: every byte is forwarded untouched, both ways.
 *  - telnet: the tool opens with the usual server set - it will echo, suppress go-ahead and
 *    send binary, and asks the client to suppress go-ahead and send binary (RFC 854, 856, 857,
 *    858) - so the client sends each key as typed, 8 bits, with no CR translation. Other options
 *    are refused. Telnet commands are taken out of the stream; IAC IAC is the data byte 0xFF.
 *  - typed bytes go to the host as they are, in order, as soon as it grants input, up to 255
 *    bytes per grant. No line editing.
 *  - ESC: a lone ESC (no other byte within --esc-ms, default 50 ms) is the escape key: the tool
 *    sends the host an ESCA, which breaks the running program, even when the host has not
 *    granted input. An ESC followed in time by more bytes (a function key such as ESC [ 4 6 _)
 *    goes to the host as typed text, unchanged. --esc-ms 0: ESC is always text.
 *  - the tool echoes typed bytes back to the client when the host's ECKM says so (the host
 *    does not echo; ECKM 0xFF, for a password, means no echo).
 *  - host text goes to the client byte for byte (0xFF doubled, as telnet needs).
 *  - keystrokes carry even parity in bit 7, as the real asker sends them (the library's
 *    keystroke_parity setting, measured); --raw-keys turns it off.
 *  - --term-type N (decimal, default 0) is the terminal type sent to the host (TTYP); real
 *    askers send 0. --user NAME (default TELNET, at most 16 characters) is the user TADA shows.
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
#include <bcrypt.h>
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
#include <termios.h>
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
/* Users file, random bytes, password from the console                                        */
/* ------------------------------------------------------------------------------------------ */

#define USERS_MAX 64

static const char *s_users_path = "host_tad_telnet.users";

/* Read every user line of the file into `out`. Returns the count, or -1 when the file cannot be
 * opened. Bad lines are reported (by line number, never their content) and skipped. */
static int users_read(AuthUser *out, int cap)
{
    char line[AUTH_LINE_MAX + 8];
    int n = 0;
    int line_no = 0;
    FILE *f = fopen(s_users_path, "r");
    if (f == NULL)
    {
        return -1;
    }
    while (fgets(line, sizeof line, f) != NULL)
    {
        AuthUser u;
        int rc;
        line_no++;
        rc = auth_user_parse_line(line, &u);
        if (rc < 0)
        {
            printf("%s line %d is not a user line; skipped\n", s_users_path, line_no);
            continue;
        }
        if (rc == 0 && n < cap)
        {
            out[n++] = u;
        }
        auth_wipe(&u, sizeof u);
    }
    auth_wipe(line, sizeof line);
    fclose(f);
    return n;
}

static int users_write(const AuthUser *u, int n)
{
    char line[AUTH_LINE_MAX];
    FILE *f = fopen(s_users_path, "w");
    if (f == NULL)
    {
        return -1;
    }
    fprintf(f, "# host_tad_telnet users: name:pbkdf2-sha256:iterations:salt:hash:flags\n");
    for (int i = 0; i < n; i++)
    {
        if (auth_user_format_line(&u[i], line, sizeof line) > 0)
        {
            fprintf(f, "%s\n", line);
        }
    }
    return (fclose(f) == 0) ? 0 : -1;
}

/* The user store port: read the file at each login, so changes apply at once. */
static int users_lookup(void *ctx, const char *name, AuthUser *out)
{
    static AuthUser all[USERS_MAX];
    int n = users_read(all, USERS_MAX);
    int found = -1;
    (void)ctx;
    for (int i = 0; i < n; i++)
    {
        if (found < 0 && strcmp(all[i].name, name) == 0)
        {
            *out = all[i];
            found = 0;
        }
    }
    auth_wipe(all, sizeof all);
    return found;
}

/* Store a record: replace the user of the same name, or add it. Returns 0 or -1. */
static int users_put(const AuthUser *u)
{
    static AuthUser all[USERS_MAX];
    int n = users_read(all, USERS_MAX);
    int at = -1;
    int rc;
    if (n < 0)
    {
        n = 0;
    }
    for (int i = 0; i < n; i++)
    {
        if (strcmp(all[i].name, u->name) == 0)
        {
            at = i;
        }
    }
    if (at < 0)
    {
        if (n >= USERS_MAX)
        {
            auth_wipe(all, sizeof all);
            return -1;
        }
        at = n++;
    }
    all[at] = *u;
    rc = users_write(all, n);
    auth_wipe(all, sizeof all);
    return rc;
}

/* The user store port for a changed password. */
static int users_save(void *ctx, const AuthUser *u)
{
    (void)ctx;
    return users_put(u);
}

static int random_fill(void *ctx, uint8_t *buf, size_t len)
{
    (void)ctx;
#ifdef _WIN32
    return (BCryptGenRandom(NULL, buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) ? 0 : -1;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got;
    if (f == NULL)
    {
        return -1;
    }
    got = fread(buf, 1, len, f);
    fclose(f);
    return (got == len) ? 0 : -1;
#endif
}

/* Read a line from the console with echo off. Returns 0, or -1 on end of input. */
static int console_password(const char *prompt, char *buf, size_t cap)
{
    int rc = 0;
    printf("%s", prompt);
    fflush(stdout);
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    bool restore = GetConsoleMode(h, &mode) != 0;
    if (restore)
    {
        SetConsoleMode(h, mode & ~(DWORD)ENABLE_ECHO_INPUT);
    }
#else
    struct termios old;
    struct termios quiet;
    bool restore = tcgetattr(0, &old) == 0;
    if (restore)
    {
        quiet = old;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(0, TCSANOW, &quiet);
    }
#endif
    if (fgets(buf, (int)cap, stdin) == NULL)
    {
        rc = -1;
    }
#ifdef _WIN32
    if (restore)
    {
        SetConsoleMode(h, mode);
    }
#else
    if (restore)
    {
        tcsetattr(0, TCSANOW, &old);
    }
#endif
    printf("\n");
    if (rc == 0)
    {
        buf[strcspn(buf, "\r\n")] = '\0';
    }
    return rc;
}

static const AuthRandom s_rng = {NULL, random_fill};

/* Ask a new password twice on the console. Returns 0 with `out` holding a record made from
 * `base` (or a new record for `name` when base is NULL) and the new password; 1 on refusal. */
static int console_new_password(const char *name, const AuthUser *base, AuthUser *out)
{
    char pw1[128];
    char pw2[128];
    int rc = 0;
    if (console_password("password: ", pw1, sizeof pw1) != 0 ||
        console_password("again: ", pw2, sizeof pw2) != 0)
    {
        rc = 1;
    }
    else if (pw1[0] == '\0')
    {
        printf("an empty password is not allowed\n");
        rc = 1;
    }
    else if (strcmp(pw1, pw2) != 0)
    {
        printf("the two passwords differ\n");
        rc = 1;
    }
    else if (base != NULL)
    {
        *out = *base;
        if (auth_user_reset_password(out, pw1, &s_rng) != AUTH_CHANGE_OK)
        {
            printf("could not set the password (random source failed)\n");
            rc = 1;
        }
    }
    else if (auth_user_make(out, name, pw1, AUTH_DEFAULT_ITERATIONS, &s_rng) != 0)
    {
        printf("could not make the user (random source failed)\n");
        rc = 1;
    }
    auth_wipe(pw1, sizeof pw1);
    auth_wipe(pw2, sizeof pw2);
    return rc;
}

static bool s_add_admin; /* --admin with --add-user */

/* --add-user, --del-user, --list-users, --reset-password. Returns the process exit code. */
static int users_command(const char *cmd, const char *name)
{
    static AuthUser all[USERS_MAX];
    int n = users_read(all, USERS_MAX);
    int rc = 0;
    if (strcmp(cmd, "--list-users") == 0)
    {
        if (n < 0)
        {
            printf("%s: no such file\n", s_users_path);
            return 1;
        }
        for (int i = 0; i < n; i++)
        {
            printf("%-32s %-5s%s\n", all[i].name,
                   (all[i].level == AUTH_LEVEL_ADMIN) ? "admin" : "user",
                   all[i].must_change ? "  must change password" : "");
        }
        auth_wipe(all, sizeof all);
        return 0;
    }
    if (n < 0)
    {
        n = 0;
    }
    if (!auth_name_is_valid(name))
    {
        printf("a user name is 1 to %u characters, no blank and no ':'\n", AUTH_NAME_MAX);
        return 1;
    }
    int at = -1;
    for (int i = 0; i < n; i++)
    {
        if (strcmp(all[i].name, name) == 0)
        {
            at = i;
        }
    }
    if (strcmp(cmd, "--del-user") == 0)
    {
        if (at < 0)
        {
            printf("no user %s in %s\n", name, s_users_path);
            return 1;
        }
        if (auth_users_is_last_admin(all, (size_t)n, name))
        {
            printf("%s is the last admin; add another admin first\n", name);
            auth_wipe(all, sizeof all);
            return 1;
        }
        all[at] = all[n - 1];
        n--;
    }
    else if (strcmp(cmd, "--reset-password") == 0)
    {
        AuthUser u;
        if (at < 0)
        {
            printf("no user %s in %s\n", name, s_users_path);
            return 1;
        }
        rc = console_new_password(name, &all[at], &u);
        if (rc != 0)
        {
            auth_wipe(all, sizeof all);
            return rc;
        }
        all[at] = u; /* must_change is now set */
        auth_wipe(&u, sizeof u);
    }
    else
    {
        AuthUser u;
        if (!s_add_admin && at >= 0 && auth_users_is_last_admin(all, (size_t)n, name))
        {
            printf("%s is the last admin; add with --admin, or add another admin first\n", name);
            auth_wipe(all, sizeof all);
            return 1;
        }
        rc = console_new_password(name, NULL, &u);
        if (rc != 0)
        {
            auth_wipe(all, sizeof all);
            return rc;
        }
        u.level = s_add_admin ? AUTH_LEVEL_ADMIN : AUTH_LEVEL_USER;
        if (at < 0)
        {
            if (n >= USERS_MAX)
            {
                printf("%s is full (%d users)\n", s_users_path, USERS_MAX);
                return 1;
            }
            at = n++;
        }
        all[at] = u;
        auth_wipe(&u, sizeof u);
    }
    if (users_write(all, n) != 0)
    {
        printf("cannot write %s\n", s_users_path);
        rc = 1;
    }
    else
    {
        printf("%s: %d user(s)\n", s_users_path, n);
    }
    auth_wipe(all, sizeof all);
    return rc;
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

#define BRIDGE_PROMPT "TAD> "

typedef enum TelnetRx
{
    TN_DATA = 0,
    TN_GOT_IAC,
    TN_GOT_VERB,
    TN_IN_SB,
    TN_IN_SB_IAC
} TelnetRx;

/* Where a client is. */
typedef enum Phase
{
    PH_LOGIN = 0, /* prompt mode: name and password */
    PH_PROMPT,    /* prompt mode: at "TAD> " */
    PH_PASSWD,    /* prompt mode: PASSWORD, changing one's own password (c->login) */
    PH_SETPW,     /* prompt mode: an admin types a password for ADDUSER or RESETPW */
    PH_SESSION    /* in a TAD session (direct mode: from the start) */
} Phase;

typedef struct Client
{
    bool used;
    Sock sock;
    TadAsker asker;
    bool connect_pending; /* waiting for the link before the letter can go */
    bool logout_seen;     /* the host said the login ended: hang up */
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
    Phase phase;
    TermLine line;   /* the bridge's own command line and login */
    AuthLogin login; /* prompt mode */
    char user[AUTH_NAME_MAX + 1u];
    AuthLevel level;       /* of the logged-in user */
    AuthUser setpw;        /* PH_SETPW: the record being made (hash only) */
    bool setpw_again;      /* PH_SETPW: the first entry is in, the second is due */
    char system[16]; /* the system of the TAD session */
    char remote[80]; /* the client's address, for WHO */
    uint32_t connected_at;
    TermEscape esc;
} Client;

typedef enum BridgeMode
{
    MODE_DIRECT = 0,
    MODE_PROMPT
} BridgeMode;

static Client s_clients[XMSG_MAX_SESSIONS];
static XmsgNode s_node;
static uint16_t s_peer;
static uint16_t s_own;
static const char *s_system;
static bool s_raw_keys; /* --raw-keys: typed bytes leave without the parity bit */
static BridgeMode s_mode = MODE_DIRECT;
static uint32_t s_esc_ms = TERM_ESCAPE_DEFAULT_MS;
static uint16_t s_term_type;
static const char *s_iam_user = "TELNET";
static char s_line_name[160]; /* the HDLC line, for INFO */
static uint32_t s_started_at;
static const AuthUserStore s_store = {NULL, users_lookup, users_save};

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

static void client_text(Client *c, const char *text)
{
    client_put(c, (const uint8_t *)text, strlen(text));
}

static void client_say(Client *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

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

static void show_prompt(Client *c)
{
    if (c->phase == PH_LOGIN)
    {
        const char *p = auth_login_prompt(&c->login);
        if (p != NULL)
        {
            c->line.echo = auth_login_echo(&c->login);
            client_text(c, p);
        }
        return;
    }
    if (c->phase == PH_PASSWD)
    {
        const char *p = auth_login_prompt(&c->login);
        c->line.echo = false;
        if (p != NULL)
        {
            client_text(c, p);
        }
        return;
    }
    if (c->phase == PH_SETPW)
    {
        c->line.echo = false;
        client_say(c, "%s for %s: ", c->setpw_again ? "again" : "new password", c->setpw.name);
        return;
    }
    if (c->phase == PH_PROMPT)
    {
        c->line.echo = true;
        client_text(c, BRIDGE_PROMPT);
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
    if (st != TAD_ASKER_CLOSED && st != TAD_ASKER_FAILED)
    {
        return;
    }
    if (!c->used)
    {
        return; /* the client is gone; this is the DCON of its hang-up being acknowledged */
    }
    client_say(c, "\r\n[session %s]\r\n", tad_asker_state_name(st));
    if (s_mode == MODE_DIRECT)
    {
        c->closing = true;
        return;
    }
    /* Prompt mode: back to the bridge's command line. */
    c->phase = PH_PROMPT;
    c->ready_len = 0;
    c->connect_pending = false;
    c->logout_seen = false;
    term_line_init(&c->line, true);
    show_prompt(c);
}

static void cb_echo(void *ctx, uint8_t eckm)
{
    ((Client *)ctx)->echo = (eckm == 0x01);
}

static void cb_logged_out(void *ctx)
{
    Client *c = (Client *)ctx;
    printf("client %d: the SINTRAN login ended\n", c->index);
    c->logout_seen = true; /* hung up from the main loop */
}

/* Prepare a TAD session to `system`; the connect letter goes once the link is ready. */
static void client_start(Client *c, const char *system)
{
    TadAskerConfig cfg;
    TadAskerCallbacks cb = {c, cb_text, cb_state, NULL, cb_echo, NULL, cb_logged_out};
    snprintf(c->system, sizeof c->system, "%s", system);
    tad_asker_config_default(&cfg, s_peer, c->system);
    cfg.keystroke_parity = !s_raw_keys;
    cfg.ttyp[0] = (uint8_t)(s_term_type >> 8u);
    cfg.ttyp[1] = (uint8_t)(s_term_type & 0xFFu);
    /* What TADA on the host shows for this session (7IAM): terminal = client slot + 1. */
    cfg.terminal_number = (uint16_t)((c - s_clients) + 1);
    cfg.user_name = s_iam_user;
    if (tad_asker_init(&c->asker, &cfg, &cb) != XMSG_OK)
    {
        client_say(c, "bad system name\r\n");
        if (s_mode == MODE_DIRECT)
        {
            c->closing = true;
        }
        show_prompt(c);
        return;
    }
    c->phase = PH_SESSION;
    c->echo = true;
    c->ready_len = 0;
    c->after_cr = false;
    c->logout_seen = false;
    term_escape_init(&c->esc, s_esc_ms);
    c->connect_pending = true;
}

static void cmd_who(Client *self, uint32_t now)
{
    TermWhoEntry e[XMSG_MAX_SESSIONS];
    char buf[256 + (XMSG_MAX_SESSIONS * 160)];
    size_t n = 0;
    for (int i = 0; i < XMSG_MAX_SESSIONS; i++)
    {
        const Client *c = &s_clients[i];
        if (!c->used)
        {
            continue;
        }
        e[n].terminal = i + 1;
        e[n].user = (s_mode == MODE_DIRECT) ? "(direct)" : ((c->phase == PH_LOGIN) ? NULL : c->user);
        e[n].remote = c->remote;
        e[n].connected_s = (now - c->connected_at) / 1000u;
        e[n].system = (c->phase == PH_SESSION) ? c->system : NULL;
        e[n].session_state = c->connect_pending ? "waiting for the link"
                                                : tad_asker_state_name(c->asker.state);
        e[n].is_self = (c == self);
        n++;
    }
    if (term_report_who(e, n, buf, sizeof buf) > 0)
    {
        client_text(self, buf);
    }
}

static void cmd_info(Client *self, uint32_t now)
{
    TermLinkInfo li;
    char buf[1024];
    const XmsgTransportPort *t = &s_node.ports.transport;
    memset(&li, 0, sizeof li);
    li.own_node = s_own;
    li.own_system = xmsg_node_system(&s_node);
    li.peer_node_learned = lapb_peer_node(&s_node.lapb);
    li.peer_node_cfg = s_peer;
    li.peer_system = s_system;
    li.link_up = xmsg_node_is_up(&s_node);
    li.link_ready = xmsg_node_is_ready(&s_node);
    li.transport = s_line_name;
    li.line_bps = (t->line_speed != NULL) ? t->line_speed(t->ctx) : 0u;
    li.line_is_tcp = true; /* this tool only dials HDLC over TCP */
    li.frames_in = s_node.lapb.stats.frames_in;
    li.frames_out = s_node.lapb.stats.frames_out;
    li.retransmissions = s_node.lapb.stats.retransmissions;
    li.unusual_address = s_node.lapb.stats.unusual_address;
    li.peer_window = s_node.lapb.peer_window;
    li.datagrams_in = s_node.stats.datagrams_in;
    li.datagrams_out = s_node.stats.datagrams_out;
    li.uptime_s = (now - s_started_at) / 1000u;
    if (term_report_info(&li, buf, sizeof buf) > 0)
    {
        client_text(self, buf);
    }
}

static void cmd_help(Client *c)
{
    client_text(c, "CONNECT <system>   connect to a SINTRAN system, e.g. CONNECT D102\r\n"
                   "LIST               the systems that can be reached\r\n"
                   "WHO                the clients on this bridge\r\n"
                   "INFO               the link to the SINTRAN machine\r\n"
                   "PASSWORD           change your password\r\n"
                   "HELP               this text\r\n"
                   "EXIT, LOGOUT, QUIT close the connection\r\n");
    if (c->level == AUTH_LEVEL_ADMIN)
    {
        client_text(c, "USERS              the users (admin)\r\n"
                       "ADDUSER name [admin]  add a user (admin)\r\n"
                       "DELUSER name       delete a user (admin)\r\n"
                       "RESETPW name       set a new password, changed at login (admin)\r\n");
    }
    client_text(c, "In a session: ESC alone breaks the running program; log out of SINTRAN to\r\n"
                   "come back here.\r\n");
}

static void cmd_users(Client *c)
{
    static AuthUser all[USERS_MAX];
    int n = users_read(all, USERS_MAX);
    for (int i = 0; i < n; i++)
    {
        client_say(c, "%-32s %-5s%s\r\n", all[i].name,
                   (all[i].level == AUTH_LEVEL_ADMIN) ? "admin" : "user",
                   all[i].must_change ? "  must change password" : "");
    }
    auth_wipe(all, sizeof all);
}

static void cmd_deluser(Client *c, const char *name)
{
    static AuthUser all[USERS_MAX];
    int n = users_read(all, USERS_MAX);
    int at = -1;
    for (int i = 0; i < n; i++)
    {
        if (strcmp(all[i].name, name) == 0)
        {
            at = i;
        }
    }
    if (at < 0)
    {
        client_say(c, "no user %s\r\n", name);
    }
    else if (auth_users_is_last_admin(all, (size_t)n, name))
    {
        client_say(c, "%s is the last admin and cannot be deleted\r\n", name);
    }
    else
    {
        all[at] = all[n - 1];
        n--;
        if (users_write(all, n) == 0)
        {
            printf("client %d (%s): deleted user %s\n", c->index, c->user, name);
            client_say(c, "user %s deleted\r\n", name);
        }
        else
        {
            client_text(c, "cannot write the users file\r\n");
        }
    }
    auth_wipe(all, sizeof all);
}

/* ADDUSER and RESETPW: check the name, then ask the password twice (PH_SETPW). */
static void cmd_setpw_begin(Client *c, const char *name, bool add, AuthLevel level)
{
    AuthUser u;
    bool exists;
    if (!auth_name_is_valid(name))
    {
        client_say(c, "a user name is 1 to %u characters, no blank and no ':'\r\n",
                   AUTH_NAME_MAX);
        return;
    }
    exists = users_lookup(NULL, name, &u) == 0;
    auth_wipe(&u, sizeof u);
    if (add && exists)
    {
        client_say(c, "user %s exists; RESETPW sets a new password\r\n", name);
        return;
    }
    if (!add && !exists)
    {
        client_say(c, "no user %s\r\n", name);
        return;
    }
    memset(&c->setpw, 0, sizeof c->setpw);
    snprintf(c->setpw.name, sizeof c->setpw.name, "%s", name);
    c->setpw.level = level;
    c->setpw.iterations = add ? 0u : 1u; /* 0: new user, made at the first entry */
    c->setpw_again = false;
    c->phase = PH_SETPW;
}

/* A line typed in PH_SETPW. The line is wiped by the caller. */
static void setpw_line(Client *c, const char *text)
{
    AuthUser u;
    if (!c->setpw_again)
    {
        int rc;
        if (text[0] == '\0')
        {
            client_text(c, "an empty password is not allowed\r\n");
            c->phase = PH_PROMPT;
            return;
        }
        if (c->setpw.iterations == 0u)
        {
            AuthLevel level = c->setpw.level;
            rc = auth_user_make(&u, c->setpw.name, text, AUTH_DEFAULT_ITERATIONS, &s_rng);
            u.level = level;
            if (rc == 0)
            {
                u.must_change = true; /* the admin knows it; the user picks their own */
            }
        }
        else
        {
            rc = (users_lookup(NULL, c->setpw.name, &u) == 0)
                     ? auth_user_reset_password(&u, text, &s_rng)
                     : -1;
        }
        if (rc != 0)
        {
            client_text(c, "could not set the password\r\n");
            auth_wipe(&u, sizeof u);
            c->phase = PH_PROMPT;
            return;
        }
        c->setpw = u;
        auth_wipe(&u, sizeof u);
        c->setpw_again = true;
        return;
    }
    if (!auth_user_check(&c->setpw, text))
    {
        client_text(c, "the two passwords differ; nothing changed\r\n");
    }
    else if (users_put(&c->setpw) != 0)
    {
        client_text(c, "cannot write the users file\r\n");
    }
    else
    {
        printf("client %d (%s): set the password of %s\n", c->index, c->user, c->setpw.name);
        client_say(c, "password of %s set; it must be changed at the next login\r\n",
                   c->setpw.name);
    }
    auth_wipe(&c->setpw, sizeof c->setpw);
    c->phase = PH_PROMPT;
}

/* A line typed in PH_PASSWD (the user's own change, through the login dialogue). */
static void passwd_line(Client *c, const char *text, uint32_t now)
{
    AuthLoginState st = auth_login_line(&c->login, text, now);
    AuthLoginNote note = auth_login_take_note(&c->login);
    if (note != AUTH_NOTE_NONE)
    {
        client_say(c, "%s\r\n", auth_login_note_text(note));
    }
    if (note == AUTH_NOTE_CHANGED)
    {
        printf("client %d (%s): changed the password\n", c->index, c->user);
    }
    if (st == AUTH_LOGIN_DONE)
    {
        c->phase = PH_PROMPT;
    }
}

/* Whether an admin command may run; tells the client when not. */
static bool admin_only(Client *c)
{
    if (c->level == AUTH_LEVEL_ADMIN)
    {
        return true;
    }
    client_text(c, "not allowed\r\n");
    return false;
}

/* One line typed at "TAD> ". */
static void prompt_line(Client *c, const char *text, uint32_t now)
{
    char arg[AUTH_NAME_MAX + 1u];
    TermCmd cmd = term_cmd_parse(text, arg, sizeof arg);
    if (strlen(arg) > AUTH_NAME_MAX)
    {
        cmd = TERM_CMD_UNKNOWN;
    }
    switch (cmd)
    {
    case TERM_CMD_EMPTY:
        break;
    case TERM_CMD_CONNECT:
        if (strlen(arg) >= sizeof c->system)
        {
            client_text(c, "unknown command; HELP lists them\r\n");
            break;
        }
        printf("client %d (%s): connect %s\n", c->index, c->user, arg);
        client_start(c, arg); /* no prompt after it: the host's banner comes */
        return;
    case TERM_CMD_LIST:
        /* TODO(ronny): LIST must ask the host we are linked to for its routing information
         * (which systems it reaches). How is being worked out; until then it says so. */
        client_text(c, "LIST: not implemented yet\r\n");
        break;
    case TERM_CMD_WHO:
        cmd_who(c, now);
        break;
    case TERM_CMD_INFO:
        cmd_info(c, now);
        break;
    case TERM_CMD_HELP:
        cmd_help(c);
        break;
    case TERM_CMD_EXIT:
        client_text(c, "bye\r\n");
        c->closing = true;
        return;
    case TERM_CMD_PASSWORD:
        if (auth_login_change_begin(&c->login) == 0)
        {
            c->phase = PH_PASSWD;
        }
        else
        {
            client_say(c, "%s\r\n", auth_login_note_text(auth_login_take_note(&c->login)));
        }
        break;
    case TERM_CMD_USERS:
        if (admin_only(c))
        {
            cmd_users(c);
        }
        break;
    case TERM_CMD_ADDUSER:
        /* fall through */
    case TERM_CMD_ADDADMIN:
        if (admin_only(c))
        {
            cmd_setpw_begin(c, arg, true,
                            (cmd == TERM_CMD_ADDADMIN) ? AUTH_LEVEL_ADMIN : AUTH_LEVEL_USER);
        }
        break;
    case TERM_CMD_DELUSER:
        if (admin_only(c))
        {
            cmd_deluser(c, arg);
        }
        break;
    case TERM_CMD_RESETPW:
        if (admin_only(c))
        {
            cmd_setpw_begin(c, arg, false, AUTH_LEVEL_USER);
        }
        break;
    default:
        client_text(c, "unknown command; HELP lists them\r\n");
        break;
    }
    show_prompt(c);
}

/* One line typed at the login. The line (maybe a password) is wiped by the caller. */
static void login_line(Client *c, const char *text, uint32_t now)
{
    AuthLoginState before = c->login.state;
    AuthLoginState st = auth_login_line(&c->login, text, now);
    AuthLoginNote note = auth_login_take_note(&c->login);
    if (note != AUTH_NOTE_NONE)
    {
        client_say(c, "%s\r\n", auth_login_note_text(note));
    }
    if (st == AUTH_LOGIN_NEW_PASSWORD && before == AUTH_LOGIN_PASSWORD)
    {
        client_text(c, "You must set a new password before going on.\r\n");
    }
    switch (st)
    {
    case AUTH_LOGIN_DONE:
        snprintf(c->user, sizeof c->user, "%s", c->login.name);
        c->level = auth_login_level(&c->login);
        printf("client %d: logged in as %s (%s)%s\n", c->index, c->user,
               (c->level == AUTH_LEVEL_ADMIN) ? "admin" : "user",
               (note == AUTH_NOTE_CHANGED) ? ", new password stored" : "");
        c->phase = PH_PROMPT;
        client_say(c, "Logged in. HELP lists the commands.\r\n");
        show_prompt(c);
        break;
    case AUTH_LOGIN_WAIT:
        printf("client %d: login failed\n", c->index);
        client_text(c, "Login incorrect\r\n");
        break;
    case AUTH_LOGIN_REFUSED:
        if (note == AUTH_NOTE_NONE)
        {
            printf("client %d: login failed; too many tries, closing\n", c->index);
            client_text(c, "Login incorrect\r\n");
        }
        else
        {
            printf("client %d: login refused: %s\n", c->index, auth_login_note_text(note));
        }
        c->closing = true;
        break;
    default:
        show_prompt(c);
        break;
    }
}

/* A byte for the bridge's own command line or login. */
static void client_line_byte(Client *c, uint8_t k, uint32_t now)
{
    uint8_t echo[3];
    size_t el = 0;
    if (c->closing || (c->phase == PH_LOGIN && c->login.state == AUTH_LOGIN_WAIT))
    {
        return; /* typed while a failed login waits: thrown away */
    }
    int done = term_line_byte(&c->line, k, echo, &el);
    client_put(c, echo, el);
    if (done != 1)
    {
        return;
    }
    char text[TERM_LINE_MAX + 1u];
    memcpy(text, c->line.text, sizeof text);
    term_line_clear(&c->line);
    if (c->phase == PH_LOGIN)
    {
        login_line(c, text, now);
    }
    else if (c->phase == PH_PASSWD)
    {
        passwd_line(c, text, now);
        show_prompt(c);
    }
    else if (c->phase == PH_SETPW)
    {
        setpw_line(c, text);
        show_prompt(c);
    }
    else
    {
        prompt_line(c, text, now);
    }
    auth_wipe(text, sizeof text);
}

/* One typed byte for the TAD session: queued for the host, in order. The telnet rule applies
 * (outside binary mode a CR is sent as CR NUL, and that NUL is not a typed byte, RFC 854),
 * then the lone-ESC rule (see the file comment). Nothing else is edited or held for Return. */
static void session_byte(Client *c, uint8_t k, uint32_t now)
{
    uint8_t out[2];
    bool escape = false;
    if (c->after_cr && k == 0x00 && !c->him_on[TN_BINARY])
    {
        c->after_cr = false;
        return;
    }
    c->after_cr = (k == '\r');
    int n = term_escape_byte(&c->esc, k, now, out, &escape);
    if (escape)
    {
        (void)tad_asker_escape(&c->asker, now); /* refused only before the session is up */
    }
    for (int i = 0; i < n; i++)
    {
        if (c->ready_len >= sizeof c->ready)
        {
            return; /* the host is not taking input; the client typed 1 KB ahead of it */
        }
        c->ready[c->ready_len++] = out[i];
        if (c->echo)
        {
            client_put(c, &out[i], 1);
        }
    }
}

static void client_byte(Client *c, uint8_t k, uint32_t now)
{
    if (c->phase == PH_SESSION)
    {
        session_byte(c, k, now);
    }
    else
    {
        client_line_byte(c, k, now);
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
static void client_rx(Client *c, const uint8_t *b, size_t n, uint32_t now)
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
                client_byte(c, x, now);
            }
            break;
        case TN_GOT_IAC:
            if (x == TN_IAC)
            {
                client_byte(c, x, now); /* IAC IAC is the data byte 0xFF */
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
    c->used = false; /* before the hang-up: its state callbacks then know the client is gone */
    /* Hang up as a real asker does when its user is done: DCON to the session port. Before
     * there is a session there is nothing to hang up, so drop it locally. */
    if (c->phase == PH_SESSION && tad_asker_hangup(&c->asker, now) != XMSG_OK)
    {
        tad_asker_abort(&c->asker);
    }
    auth_wipe(&c->line, sizeof c->line);
    auth_wipe(&c->login, sizeof c->login);
    auth_wipe(&c->setpw, sizeof c->setpw);
    sock_close(c->sock);
}

/* The client's address as "IP:port", for WHO. */
static void peer_address(Sock s, char *out, size_t cap)
{
    struct sockaddr_storage sa;
    socklen_t len = sizeof sa;
    char ip[INET6_ADDRSTRLEN];
    unsigned port = 0;
    snprintf(out, cap, "?");
    if (getpeername(s, (struct sockaddr *)&sa, &len) != 0)
    {
        return;
    }
    if (sa.ss_family == AF_INET)
    {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)&sa;
        if (inet_ntop(AF_INET, &v4->sin_addr, ip, sizeof ip) == NULL)
        {
            return;
        }
        port = ntohs(v4->sin_port);
    }
    else
    {
        return;
    }
    snprintf(out, cap, "%s:%u", ip, port);
}

static void client_accept(Sock lsock, uint32_t now)
{
    Sock cs = accept(lsock, NULL, NULL);
    int i;
    if (cs == SOCK_NONE)
    {
        return;
    }
    /* A slot is free once its client is gone AND its session has let go of the node: after a
     * hang-up the DCON is still in flight on that session. */
    for (i = 0; i < XMSG_MAX_SESSIONS && (s_clients[i].used || s_clients[i].asker.base.attached);
         i++)
    {
    }
    if (i == XMSG_MAX_SESSIONS)
    {
        static const char full[] = "all sessions in use\r\n";
        (void)send(cs, full, (int)sizeof full - 1, 0);
        sock_close(cs);
        return;
    }
    Client *c = &s_clients[i];
    memset(c, 0, sizeof *c);
    c->used = true;
    c->sock = cs;
    c->index = i;
    c->echo = true;
    c->connected_at = now;
    peer_address(cs, c->remote, sizeof c->remote);
    sock_nonblocking(cs);
    tn_open(c);
    printf("client %d: connected from %s\n", i, c->remote);
    if (s_mode == MODE_DIRECT)
    {
        client_start(c, s_system);
        return;
    }
    (void)auth_login_init(&c->login, &s_store, &s_rng, AUTH_LOGIN_TRIES, AUTH_LOGIN_DELAY_MS);
    term_line_init(&c->line, true);
    c->phase = PH_LOGIN;
    client_say(c, "libxmsg TAD bridge, node %u, linked to %s\r\n", (unsigned)s_own, s_system);
    show_prompt(c);
}

/* Per-client work of one loop turn, after its socket was read. */
static void client_service(Client *c, uint32_t now)
{
    if (c->phase == PH_LOGIN && auth_login_tick(&c->login, now))
    {
        show_prompt(c);
    }
    if (c->phase != PH_SESSION)
    {
        return;
    }
    if (term_escape_poll(&c->esc, now))
    {
        (void)tad_asker_escape(&c->asker, now); /* refused only before the session is up */
    }
    if (c->logout_seen)
    {
        /* The SINTRAN login ended: hang up (DCON). The state callback then closes the client
         * (direct mode) or returns it to the prompt (prompt mode). */
        c->logout_seen = false;
        if (tad_asker_hangup(&c->asker, now) != XMSG_OK)
        {
            tad_asker_abort(&c->asker);
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
            if (s_mode == MODE_DIRECT)
            {
                c->closing = true;
            }
            else
            {
                c->phase = PH_PROMPT;
                show_prompt(c);
            }
        }
    }
    client_feed_host(c, now);
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
           "         [--listen PORT]      telnet port (2300)\n"
           "         [--mode direct|prompt] direct: straight into a session on --system\n"
           "                              prompt: login, then the TAD> command line\n"
           "         [--users FILE]       users for prompt mode (host_tad_telnet.users)\n"
           "         [--esc-ms N]         a lone ESC within N ms is escape (50; 0 = never)\n"
           "         [--term-type N]      terminal type sent to the host, decimal (0)\n"
           "         [--user NAME]        the user TADA shows, at most 16 characters (TELNET)\n"
           "         [--raw-keys]         no parity bit on typed bytes\n"
           "         [--flags1 FILE]      Flags1 counts (host_tad_telnet.flags1)\n"
           "         [--log error|warn|info|debug|trace]\n"
           "       host_tad_telnet --add-user NAME [--admin] | --del-user NAME | --list-users\n"
           "                     | --reset-password NAME   [--users FILE]\n"
           "         --add-user      add or replace a user (level user, or admin with --admin)\n"
           "         --del-user      delete a user (not the last admin)\n"
           "         --list-users    names, level, and who must change the password\n"
           "         --reset-password  new password (asked twice), changed at the next login\n"
           "users file: one line per user,\n"
           "  name:pbkdf2-sha256:iterations:salt hex:hash hex:admin|user[,change]\n"
           "prompt mode with no users file creates one with admin/admin (must change).\n");
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

typedef struct Args
{
    const char *hdlc;
    uint16_t listen_port;
    XmsgLogLevel level;
    const char *flags1_path;
    const char *users_cmd; /* --add-user, --del-user, --list-users, --reset-password */
    const char *users_name;
} Args;

/* Read one "--key value" pair. Returns false for an unknown key or a bad value. */
static bool parse_pair(Args *a, const char *k, const char *v)
{
    uint16_t n = 0;
    if (strcmp(k, "--hdlc") == 0)
    {
        a->hdlc = v;
    }
    else if (strcmp(k, "--node") == 0)
    {
        return parse_u16(v, &s_own);
    }
    else if (strcmp(k, "--peer") == 0)
    {
        return parse_u16(v, &s_peer);
    }
    else if (strcmp(k, "--system") == 0)
    {
        s_system = v;
    }
    else if (strcmp(k, "--listen") == 0)
    {
        return parse_u16(v, &a->listen_port);
    }
    else if (strcmp(k, "--flags1") == 0)
    {
        a->flags1_path = v;
    }
    else if (strcmp(k, "--users") == 0)
    {
        s_users_path = v;
    }
    else if (strcmp(k, "--mode") == 0)
    {
        if (strcmp(v, "direct") != 0 && strcmp(v, "prompt") != 0)
        {
            return false;
        }
        s_mode = (strcmp(v, "prompt") == 0) ? MODE_PROMPT : MODE_DIRECT;
    }
    else if (strcmp(k, "--esc-ms") == 0)
    {
        if (!parse_u16(v, &n))
        {
            return false;
        }
        s_esc_ms = n;
    }
    else if (strcmp(k, "--term-type") == 0)
    {
        return parse_u16(v, &s_term_type);
    }
    else if (strcmp(k, "--user") == 0)
    {
        s_iam_user = v;
        return strlen(v) > 0 && strlen(v) < TAD_IAM_USER_SIZE; /* room for the "'" */
    }
    else if (strcmp(k, "--add-user") == 0 || strcmp(k, "--del-user") == 0 ||
             strcmp(k, "--reset-password") == 0)
    {
        a->users_cmd = k;
        a->users_name = v;
    }
    else if (strcmp(k, "--log") == 0)
    {
        static const char *const lv[] = {"error", "warn", "info", "debug", "trace"};
        for (int j = 0; j < 5; j++)
        {
            if (strcmp(v, lv[j]) == 0)
            {
                a->level = (XmsgLogLevel)j;
                return true;
            }
        }
        return false;
    }
    else
    {
        return false;
    }
    return true;
}

static bool parse_args(Args *a, int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--raw-keys") == 0)
        {
            s_raw_keys = true;
            continue;
        }
        if (strcmp(argv[i], "--admin") == 0)
        {
            s_add_admin = true;
            continue;
        }
        if (strcmp(argv[i], "--list-users") == 0)
        {
            a->users_cmd = argv[i];
            continue;
        }
        if (i + 1 >= argc || !parse_pair(a, argv[i], argv[i + 1]))
        {
            return false;
        }
        i++;
    }
    return true;
}

/* The line: read what came, and on a loss tell every client. */
static void line_read(uint8_t *buf, size_t cap, uint32_t now)
{
    int n = sock_recv(s_line, buf, cap);
    if (n > 0)
    {
        xmsg_node_feed(&s_node, buf, (size_t)n, now);
        return;
    }
    if (n == 0)
    {
        return;
    }
    printf("line lost\n");
    sock_close(s_line);
    s_line = SOCK_NONE;
    xmsg_node_transport_lost(&s_node);
    for (int i = 0; i < XMSG_MAX_SESSIONS; i++)
    {
        Client *c = &s_clients[i];
        if (c->used && c->phase == PH_SESSION)
        {
            client_say(c, "\r\n[line lost]\r\n");
            tad_asker_abort(&c->asker);
        }
    }
}

static int node_setup(FileStore *store)
{
    XmsgNodeConfig cfg;
    XmsgPorts ports;
    XmsgNodeCallbacks cb;
    xmsg_node_config_default(&cfg, s_own);
    memset(&ports, 0, sizeof ports);
    ports.transport.write = line_write;
    ports.transport.line_speed = NULL; /* a TCP stream has no line speed */
    ports.flags1.ctx = store;
    ports.flags1.load = store_load;
    ports.flags1.save = store_save;
    ports.flags1.load_port_seed = store_load_seed;
    ports.flags1.save_port_seed = store_save_seed;
    ports.log.write = log_line;
    memset(&cb, 0, sizeof cb);
    cb.link_state = on_link;
    return xmsg_node_init(&s_node, &cfg, &ports, &cb);
}

static void serve(Sock lsock, const char *host, const char *port)
{
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
            line_read(buf, sizeof buf, now);
        }
        if (FD_ISSET(lsock, &rd))
        {
            client_accept(lsock, now);
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
                    client_rx(c, buf, (size_t)n, now);
                }
            }
            client_service(c, now);
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

int main(int argc, char **argv)
{
    Args a = {NULL, 2300, XMSG_LOG_INFO, "host_tad_telnet.flags1", NULL, NULL};
    FileStore store = {"host_tad_telnet.flags1", 0, {0}, {0}, 0, 0, false};
    char host[128];
    char port[16];

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!parse_args(&a, argc, argv))
    {
        usage();
        return 1;
    }
    if (a.users_cmd != NULL)
    {
        return users_command(a.users_cmd, a.users_name);
    }
    const char *colon = (a.hdlc != NULL) ? strrchr(a.hdlc, ':') : NULL;
    if (colon == NULL || s_own == 0 || s_peer == 0 || s_system == NULL ||
        (size_t)(colon - a.hdlc) >= sizeof host || strlen(colon + 1) >= sizeof port)
    {
        usage();
        return 1;
    }
    memcpy(host, a.hdlc, (size_t)(colon - a.hdlc));
    host[colon - a.hdlc] = '\0';
    snprintf(port, sizeof port, "%s", colon + 1);
    snprintf(s_line_name, sizeof s_line_name, "HDLC over TCP %s:%s", host, port);
    if (s_mode == MODE_PROMPT)
    {
        static AuthUser check[USERS_MAX];
        int n = users_read(check, USERS_MAX);
        auth_wipe(check, sizeof check);
        if (n <= 0)
        {
            AuthUser admin;
            if (auth_user_make_default_admin(&admin, AUTH_DEFAULT_ITERATIONS, &s_rng) != 0 ||
                users_write(&admin, 1) != 0)
            {
                auth_wipe(&admin, sizeof admin);
                printf("%s %s, and a default admin could not be written to it\n", s_users_path,
                       (n < 0) ? "does not exist" : "holds no user");
                return 1;
            }
            auth_wipe(&admin, sizeof admin);
            printf("WARNING: %s %s. Created it with the default admin:\n"
                   "WARNING:   name admin, password admin.\n"
                   "WARNING: Log in as admin at once; the password must be changed at the\n"
                   "WARNING: first login, before anything else works.\n",
                   s_users_path, (n < 0) ? "did not exist" : "held no user");
            n = 1;
        }
        printf("prompt mode: %d user(s) in %s\n", n, s_users_path);
    }

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        printf("Winsock did not start\n");
        return 1;
    }
#endif
    store.path = a.flags1_path;
    store.own = s_own;
    store_read(&store);
    xmsg_log_set_all_levels(a.level);
    if (node_setup(&store) != XMSG_OK)
    {
        printf("node set-up failed\n");
        return 1;
    }
    Sock lsock = sock_listen(a.listen_port);
    if (lsock == SOCK_NONE)
    {
        printf("cannot listen on port %u\n", a.listen_port);
        return 1;
    }
    s_started_at = now_ms();
    printf("telnet to port %u (%s mode); line %s:%s, we are node %u, host node %u (%s)\n",
           a.listen_port, (s_mode == MODE_PROMPT) ? "prompt" : "direct", host, port, s_own,
           s_peer, s_system);
    serve(lsock, host, port);
    return 0;
}
