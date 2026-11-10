/*
 * host_tad_telnet.c - telnet to a SINTRAN machine, and CONNECT-TO from one, on one command line.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * The tool dials the HDLC line of a SINTRAN machine (nd100x started with --hdlc=N:PORT listens
 * for it), brings the XMSG link up as node --node, and listens for telnet clients. A SINTRAN
 * user may also call us (CONNECT-TO with our node's system name): that caller gets the same
 * login and command line as a telnet user. Both kinds share HOST_PLACES places (8); up to
 * HOST_RESPONDERS (4) SINTRAN calls at once. The login and the command line are the library's
 * console module (src/libxmsg/console/console.h); the telnet option handling is its telnet
 * codec (term/term_telnet.h). This file only owns the sockets, the files and the node, and
 * implements the console's ports on its single thread.
 *
 *   host_tad_telnet --hdlc 127.0.0.1:1362 --node 19999 --peer 100 --system D100 --listen 2300
 *                   [--mode direct|prompt] [--users FILE] [--esc-ms N] [--term-type N]
 *                   [--user NAME] [--raw-keys] [--flags1 flags1.txt] [--log info]
 *                   [--route NAME=N ...]
 *   host_tad_telnet --add-user NAME [--admin] | --del-user NAME | --list-users
 *                   | --reset-password NAME   [--users FILE]
 *
 * The SINTRAN side must know our node: the link is defined there for it, and XMSG is running.
 *
 * System names: CONNECT <name> addresses the TARGET system's own number, not the machine at the
 * other end of our line (tad_asker.h peer_node); the machines between pass the call on. The
 * number comes from our own table, as SINTRAN's DEF-REMOTE: --system NAME is always in it with
 * --peer, and each --route NAME=N adds one (for example --route D102=102). A name that is not in
 * the table is refused at the prompt ("unknown system"). The number is never read from the name.
 * CONNECT also takes a plain system number (CONNECT 102): Ronny's rule is "a system number or an
 * alias, never a number read out of a name".
 *
 * Two modes (--mode):
 *  - direct (the default): each telnet client goes straight into a TAD session on --system.
 *    When the SINTRAN login ends (the host's SYCN 000B after LOGOUT), the tool hangs up the TAD
 *    session (DCON) and closes the telnet connection.
 *  - prompt: each telnet client first logs in to the tool ("login: ", "password: "; users from
 *    the --users file), then gets the tool's own command line "TAD> ":
 *      CONNECT <system>   a TAD session on that system (the connect letter names it; the host
 *                         we are linked to routes it on)
 *      LIST               the routing table of the host we are linked to, read with XSGSY
 *                         (xrout_walk.h): system, how it is reached, detail, hops and WANs
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
 *  - in both modes, when the telnet connection drops (or the SINTRAN caller hangs up), the TAD
 *    session is hung up at once, which logs the user out of SINTRAN.
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
 *    does not echo; ECKM 0xFF, for a password, means no echo). Not the Enter key (the host
 *    sends its own CR LF for it) and not ESC; BS and DEL echo as BS SP BS (console.c).
 *  - a SINTRAN caller's own machine echoes, and its escape key arrives as ESCA, which goes on
 *    to the onward host as an ESCA.
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
static int tty_password(const char *prompt, char *buf, size_t cap)
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
    if (tty_password("password: ", pw1, sizeof pw1) != 0 ||
        tty_password("again: ", pw2, sizeof pw2) != 0)
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

/* The users file as the console's user list (USERS, DELUSER). */
static int users_get(void *ctx, size_t index, AuthUser *out)
{
    static AuthUser all[USERS_MAX];
    int n = users_read(all, USERS_MAX);
    int rc = -1;
    (void)ctx;
    if (n > 0 && index < (size_t)n)
    {
        *out = all[index];
        rc = 0;
    }
    auth_wipe(all, sizeof all);
    return rc;
}

static int users_remove(void *ctx, const char *name)
{
    static AuthUser all[USERS_MAX];
    int n = users_read(all, USERS_MAX);
    int rc = -1;
    (void)ctx;
    for (int i = 0; i < n; i++)
    {
        if (strcmp(all[i].name, name) == 0)
        {
            all[i] = all[n - 1];
            rc = users_write(all, n - 1);
            break;
        }
    }
    auth_wipe(all, sizeof all);
    return rc;
}

/* ------------------------------------------------------------------------------------------ */
/* System names: our own table, as SINTRAN's DEF-REMOTE                                       */
/* ------------------------------------------------------------------------------------------ */

#define ROUTES_MAX 16

typedef struct Route
{
    char name[CONSOLE_SYSTEM_MAX + 1u];
    uint16_t number;
} Route;

static Route s_routes[ROUTES_MAX];
static int s_route_count;

/* Add NAME=NUMBER (case kept as typed; looked up without regard to case). */
static bool route_add(const char *name, size_t len, uint16_t number)
{
    if (len == 0 || len > CONSOLE_SYSTEM_MAX || s_route_count >= ROUTES_MAX)
    {
        return false;
    }
    for (size_t i = 0; i < len; i++)
    {
        char ch = name[i];
        s_routes[s_route_count].name[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
    }
    s_routes[s_route_count].name[len] = '\0';
    s_routes[s_route_count].number = number;
    s_route_count++;
    return true;
}

/* The console's resolve: a typed name to the TARGET system's number (tad_asker.h peer_node).
 * Never worked out from the name text. CONNECT arrives in upper case (term_cmd_parse). */
static int svc_resolve(void *ctx, const char *system, uint16_t *number)
{
    char up[CONSOLE_SYSTEM_MAX + 1u];
    size_t n = strlen(system);
    (void)ctx;
    if (n > CONSOLE_SYSTEM_MAX)
    {
        return -1;
    }
    for (size_t i = 0; i <= n; i++)
    {
        char ch = system[i];
        up[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
    }
    for (int i = 0; i < s_route_count; i++)
    {
        if (strcmp(s_routes[i].name, up) == 0)
        {
            *number = s_routes[i].number;
            return 0;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------------------------------ */
/* Places: one onward TAD session and one LIST walk per console place                         */
/* ------------------------------------------------------------------------------------------ */

#define HOST_PLACES     8 /* users at once, telnet and SINTRAN callers together */
#define HOST_RESPONDERS 4 /* SINTRAN calls being set up or running at once */
#define HOST_CLIENTS    HOST_PLACES

/* Node sessions: one onward session or one LIST per place at a time, two per responder. A
 * hang-up still in flight can hold one more for a moment. */
#if XMSG_MAX_SESSIONS < (HOST_PLACES + (2 * HOST_RESPONDERS))
#error "XMSG_MAX_SESSIONS is too small for HOST_PLACES and HOST_RESPONDERS"
#endif

typedef struct Place
{
    TadAsker asker;
    char system[CONSOLE_SYSTEM_MAX + 1u];
    char user[TAD_IAM_USER_SIZE];
    bool pending;       /* waiting for the link before the connect letter can go */
    bool running_seen;  /* RUNNING not yet told to the console */
    bool ended;         /* CLOSED/FAILED not yet told to the console */
    ConsoleOnward end_state;
    int end_error;
    bool logout;        /* the host said its login ended */
    uint8_t ready[1024]; /* typed bytes waiting for an input grant */
    size_t ready_len;
    XroutWalk walk;
    bool listing;
} Place;

typedef enum BridgeMode
{
    MODE_DIRECT = 0,
    MODE_PROMPT
} BridgeMode;

static Place s_places[HOST_PLACES];
static ConsoleSession s_sessions[HOST_PLACES];
static ConsoleHost s_console;
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
static char s_banner[160];
static uint32_t s_started_at;
static const AuthUserStore s_store = {NULL, users_lookup, users_save};

/* Asker callbacks. Text and echo come only from the node's input, so they may feed the console
 * at once. A state change can come from inside tad_asker_abort(), which the console's
 * onward_hangup calls, so it is recorded and fed from the loop (console.h rule). */
static void asker_text(void *ctx, const uint8_t *bytes, size_t len)
{
    Place *p = (Place *)ctx;
    console_onward_text(console_host_session(&s_console, (int)(p - s_places)), bytes, len);
}

static void asker_echo(void *ctx, uint8_t eckm)
{
    Place *p = (Place *)ctx;
    console_onward_echo(console_host_session(&s_console, (int)(p - s_places)), eckm == 0x01u);
}

static void asker_state(void *ctx, TadAskerState st, int error)
{
    Place *p = (Place *)ctx;
    printf("place %d: session %s", (int)(p - s_places), tad_asker_state_name(st));
    if (st == TAD_ASKER_FAILED)
    {
        printf(" (error %d%s%s)", error, (error > 0) ? ": " : "",
               (error > 0) ? xrout_error_name((uint8_t)error) : "");
    }
    printf("\n");
    if (st == TAD_ASKER_RUNNING)
    {
        p->running_seen = true;
    }
    else if (st == TAD_ASKER_CLOSED || st == TAD_ASKER_FAILED)
    {
        p->ended = true;
        p->end_state = (st == TAD_ASKER_FAILED) ? CONSOLE_ONWARD_FAILED : CONSOLE_ONWARD_CLOSED;
        p->end_error = error;
    }
}

static void asker_logged_out(void *ctx)
{
    Place *p = (Place *)ctx;
    printf("place %d: the SINTRAN login ended\n", (int)(p - s_places));
    p->logout = true;
}

/* ------------------------------------------------------------------------------------------ */
/* The console's services port, on this single thread                                         */
/* ------------------------------------------------------------------------------------------ */

static int svc_open(void *ctx, int place, const char *system, uint16_t number,
                    const char *tad_user)
{
    Place *p = &s_places[place];
    TadAskerConfig cfg;
    TadAskerCallbacks cb = {p, asker_text, asker_state, NULL, asker_echo, NULL, asker_logged_out};
    (void)ctx;
    if (p->asker.base.attached)
    {
        return XMSG_EAGAIN; /* the last session on this place is still hanging up */
    }
    snprintf(p->system, sizeof p->system, "%s", system);
    snprintf(p->user, sizeof p->user, "%s", (tad_user != NULL) ? tad_user : s_iam_user);
    /* Addressed to the TARGET's own number, not the link neighbour (tad_asker.h peer_node). */
    tad_asker_config_default(&cfg, number, p->system);
    cfg.keystroke_parity = !s_raw_keys;
    cfg.ttyp[0] = (uint8_t)(s_term_type >> 8u);
    cfg.ttyp[1] = (uint8_t)(s_term_type & 0xFFu);
    /* What TADA on the host shows for this session (7IAM): terminal = place + 1. */
    cfg.terminal_number = (uint16_t)(place + 1);
    cfg.user_name = p->user;
    if (tad_asker_init(&p->asker, &cfg, &cb) != XMSG_OK)
    {
        return XMSG_EINVAL;
    }
    p->ready_len = 0;
    p->running_seen = false;
    p->ended = false;
    p->logout = false;
    p->pending = true; /* the letter goes once the link is ready (place_service) */
    return 0;
}

static int svc_write(void *ctx, int place, const uint8_t *bytes, size_t len)
{
    Place *p = &s_places[place];
    size_t room = sizeof p->ready - p->ready_len;
    size_t n = (len < room) ? len : room;
    (void)ctx;
    memcpy(p->ready + p->ready_len, bytes, n);
    p->ready_len += n;
    return (int)n;
}

static void svc_escape(void *ctx, int place)
{
    (void)ctx;
    (void)tad_asker_escape(&s_places[place].asker, now_ms()); /* refused only before it is up */
}

static void svc_hangup(void *ctx, int place)
{
    Place *p = &s_places[place];
    (void)ctx;
    if (p->pending)
    {
        p->pending = false; /* the letter never went: nothing to hang up */
        p->ended = true;
        p->end_state = CONSOLE_ONWARD_CLOSED;
        p->end_error = 0;
        return;
    }
    /* As a real asker does when its user is done: DCON to the session port. */
    if (tad_asker_hangup(&p->asker, now_ms()) != XMSG_OK)
    {
        tad_asker_abort(&p->asker); /* its state callback marks the end */
        if (!p->ended)
        {
            p->ended = true;
            p->end_state = CONSOLE_ONWARD_CLOSED;
            p->end_error = 0;
        }
    }
}

static void walk_entry(void *ctx, const XroutRouteEntry *e)
{
    Place *p = (Place *)ctx;
    if (p->listing)
    {
        console_list_entry(console_host_session(&s_console, (int)(p - s_places)), e);
    }
}

static void walk_done(void *ctx, int result, unsigned entries)
{
    Place *p = (Place *)ctx;
    if (!p->listing)
    {
        return; /* cancelled: the user went away */
    }
    p->listing = false;
    console_list_done(console_host_session(&s_console, (int)(p - s_places)), result, entries);
}

static int svc_list(void *ctx, int place)
{
    Place *p = &s_places[place];
    XroutWalkCallbacks cb = {p, walk_entry, walk_done};
    int rc;
    (void)ctx;
    /* The host's XMSG system number is taken to be its node number, as on every captured
     * machine (xmsg_node.h, "Routing questions"). */
    rc = xrout_walk_start(&p->walk, &s_node, s_peer, s_peer, 10000, &cb, now_ms());
    p->listing = (rc == XMSG_OK);
    return rc;
}

static void svc_list_cancel(void *ctx, int place)
{
    Place *p = &s_places[place];
    (void)ctx;
    p->listing = false; /* first: the cancel calls walk_done, which then tells nobody */
    (void)xrout_walk_cancel(&p->walk);
}

static void svc_link_info(void *ctx, TermLinkInfo *li)
{
    const XmsgTransportPort *t = &s_node.ports.transport;
    (void)ctx;
    li->own_node = s_own;
    li->own_system = xmsg_node_system(&s_node);
    li->peer_node_learned = lapb_peer_node(&s_node.lapb);
    li->peer_node_cfg = s_peer;
    li->peer_system = s_system;
    li->link_up = xmsg_node_is_up(&s_node);
    li->link_ready = xmsg_node_is_ready(&s_node);
    li->transport = s_line_name;
    li->line_bps = (t->line_speed != NULL) ? t->line_speed(t->ctx) : 0u;
    li->line_is_tcp = true; /* this tool only dials HDLC over TCP */
    li->frames_in = s_node.lapb.stats.frames_in;
    li->frames_out = s_node.lapb.stats.frames_out;
    li->retransmissions = s_node.lapb.stats.retransmissions;
    li->unusual_address = s_node.lapb.stats.unusual_address;
    li->peer_window = s_node.lapb.peer_window;
    li->datagrams_in = s_node.stats.datagrams_in;
    li->datagrams_out = s_node.stats.datagrams_out;
    li->uptime_s = (now_ms() - s_started_at) / 1000u;
}

/* Per-place work of one loop turn: feed the recorded events, send the letter, type. */
static void place_service(int i, uint32_t now)
{
    Place *p = &s_places[i];
    ConsoleSession *s = console_host_session(&s_console, i);
    if (p->pending && xmsg_node_is_ready(&s_node))
    {
        int rc = tad_asker_connect(&p->asker, &s_node, now);
        if (rc == XMSG_OK)
        {
            p->pending = false;
        }
        else if (rc != XMSG_ESTATE)
        {
            p->pending = false;
            p->ended = true;
            p->end_state = CONSOLE_ONWARD_FAILED;
            p->end_error = rc; /* no free session on the node */
        }
    }
    if (p->running_seen)
    {
        p->running_seen = false;
        console_onward_state(s, CONSOLE_ONWARD_RUNNING, 0);
    }
    if (p->logout)
    {
        p->logout = false;
        console_onward_logout(s);
    }
    if (p->ended)
    {
        p->ended = false;
        p->ready_len = 0;
        console_onward_state(s, p->end_state, p->end_error);
    }
    /* Typed bytes go as soon as the host lets us type: up to TAD_MAX_DATA per grant. */
    if (p->ready_len > 0 && tad_asker_may_type(&p->asker))
    {
        size_t n = (p->ready_len > TAD_MAX_DATA) ? TAD_MAX_DATA : p->ready_len;
        int took = tad_asker_write(&p->asker, p->ready, n, now);
        if (took > 0)
        {
            memmove(p->ready, p->ready + took, p->ready_len - (size_t)took);
            p->ready_len -= (size_t)took;
        }
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Telnet clients: a socket, the telnet codec and a console line                              */
/* ------------------------------------------------------------------------------------------ */

typedef struct Client
{
    bool used;
    Sock sock;
    TermTelnet tn;
    ConsoleSession *s; /* NULL once the console closed the line */
    uint8_t out[16384]; /* bytes for the client socket */
    size_t out_len;
    bool closing; /* flush and close */
    char remote[64];
} Client;

static Client s_clients[HOST_CLIENTS];

static void client_raw(Client *c, const uint8_t *b, size_t n)
{
    if (c->out_len + n <= sizeof c->out)
    {
        memcpy(c->out + c->out_len, b, n);
        c->out_len += n;
    }
}

static int client_write(void *ctx, const uint8_t *b, size_t n)
{
    Client *c = (Client *)ctx;
    size_t taken = 0;
    int o = term_telnet_escape(b, n, c->out + c->out_len, sizeof c->out - c->out_len, &taken);
    if (o > 0)
    {
        c->out_len += (size_t)o; /* the client is not reading: the rest is dropped */
    }
    return (int)taken;
}

static void client_close(void *ctx, ConsoleClose why)
{
    Client *c = (Client *)ctx;
    (void)why;
    c->s = NULL;
    c->closing = true;
}

static void client_describe(void *ctx, char *buf, size_t cap)
{
    snprintf(buf, cap, "%s", ((const Client *)ctx)->remote);
}

/* The client's address as "IP:port", for WHO. */
static void peer_address(Sock s, char *out, size_t cap)
{
    struct sockaddr_storage sa;
    socklen_t len = sizeof sa;
    char ip[INET_ADDRSTRLEN]; /* IPv4 only: the listener is AF_INET */
    snprintf(out, cap, "?");
    if (getpeername(s, (struct sockaddr *)&sa, &len) != 0 || sa.ss_family != AF_INET)
    {
        return;
    }
    const struct sockaddr_in *v4 = (const struct sockaddr_in *)&sa;
    if (inet_ntop(AF_INET, &v4->sin_addr, ip, sizeof ip) == NULL)
    {
        return;
    }
    snprintf(out, cap, "%s:%u", ip, (unsigned)ntohs(v4->sin_port));
}

static void client_accept(Sock lsock, uint32_t now)
{
    static const char full[] = "all sessions in use\r\n";
    Sock cs = accept(lsock, NULL, NULL);
    ConsoleLine line;
    int i;
    if (cs == SOCK_NONE)
    {
        return;
    }
    for (i = 0; i < HOST_CLIENTS && s_clients[i].used; i++)
    {
    }
    if (i == HOST_CLIENTS)
    {
        (void)send(cs, full, (int)sizeof full - 1, 0);
        sock_close(cs);
        return;
    }
    Client *c = &s_clients[i];
    memset(c, 0, sizeof *c);
    c->used = true;
    c->sock = cs;
    peer_address(cs, c->remote, sizeof c->remote);
    sock_nonblocking(cs);
    term_telnet_init(&c->tn, true);
    c->out_len = (size_t)term_telnet_open(&c->tn, c->out, sizeof c->out);
    memset(&line, 0, sizeof line);
    line.ctx = c;
    line.kind = "telnet";
    line.write = client_write;
    line.close = client_close;
    line.describe = client_describe;
    c->s = console_host_open(&s_console, &line, now);
    if (c->s == NULL && !c->closing)
    {
        client_raw(c, (const uint8_t *)full, sizeof full - 1);
        c->closing = true;
    }
    printf("telnet client %d: connected from %s\n", i, c->remote);
}

/* Take telnet commands out; hand the data bytes to the console. */
static void client_rx(Client *c, const uint8_t *b, size_t n, uint32_t now)
{
    uint8_t data[2048];
    size_t dn = 0;
    for (size_t i = 0; i < n; i++)
    {
        uint8_t reply[TERM_TELNET_REPLY_MAX];
        size_t rl = 0;
        int d = term_telnet_rx(&c->tn, b[i], reply, &rl);
        client_raw(c, reply, rl);
        if (d >= 0 && dn < sizeof data)
        {
            data[dn++] = (uint8_t)d;
        }
    }
    if (c->s != NULL && dn > 0)
    {
        console_input(c->s, data, dn, now);
    }
}

static void client_drop(Client *c)
{
    printf("telnet client %d: gone\n", (int)(c - s_clients));
    if (c->s != NULL)
    {
        console_line_lost(c->s); /* hangs up its session: that logs the user out of SINTRAN */
        c->s = NULL;
    }
    c->used = false;
    sock_close(c->sock);
}

/* ------------------------------------------------------------------------------------------ */
/* SINTRAN callers (CONNECT-TO this node): the same console, on a TAD line                    */
/* ------------------------------------------------------------------------------------------ */

static ConsoleTadLine s_tad[HOST_RESPONDERS];

static int tad_incoming(void *ctx, const TadResponderCaller *who)
{
    uint16_t used = 0;
    uint16_t total = 0;
    (void)ctx;
    console_host_counts(&s_console, &used, &total);
    if (used >= total)
    {
        printf("SINTRAN caller from system %u refused: every place in use\n",
               (unsigned)who->system);
        return 4; /* XRNSP: no free seat (tad_responder.h) */
    }
    return 0;
}

static void tad_status(void *ctx, uint16_t *in_use, uint16_t *total)
{
    (void)ctx;
    console_host_counts(&s_console, in_use, total);
}

static void tad_setup(void)
{
    for (int k = 0; k < HOST_RESPONDERS; k++)
    {
        ConsoleTadConfig tc;
        console_tad_config_default(&tc);
        tc.incoming = tad_incoming;
        tc.status = tad_status;
        /* Each responder its own logical unit, 0x0300 + n as captured hosts do
         * (tad_responder.h: meaning inferred, not decoded). */
        tc.responder.logical_unit = (uint16_t)(0x0300u + (uint16_t)k);
        (void)console_tad_init(&s_tad[k], &tc);
    }
}

/* The node's on_letter: offer a port-0 letter to the lines. */
static void on_letter(void *ctx, const XmsgDatagram *dg, uint32_t now)
{
    (void)ctx;
    for (int k = 0; k < HOST_RESPONDERS; k++)
    {
        int rc = console_tad_offer(&s_tad[k], &s_node, dg, now);
        if (rc != XMSG_EAGAIN)
        {
            return; /* handled, not a TAD letter, or an error the line has logged */
        }
    }
    if (tad_responder_is_status_query(dg))
    {
        uint16_t used = 0;
        uint16_t total = 0;
        console_host_counts(&s_console, &used, &total);
        (void)tad_responder_status_reply(&s_node, dg, used, (uint16_t)(total - used), now);
        return;
    }
    printf("SINTRAN caller refused: all %d responders busy\n", HOST_RESPONDERS);
    (void)tad_responder_refuse(&s_node, dg, 4u, now);
}

/* Console side of each TAD line: open a session when a call runs, feed it, free the line. */
static void tad_service(uint32_t now)
{
    for (int k = 0; k < HOST_RESPONDERS; k++)
    {
        ConsoleTadLine *t = &s_tad[k];
        if (console_tad_needs_session(t))
        {
            ConsoleLine line;
            ConsoleSession *s;
            console_tad_line(t, &line);
            s = console_host_open(&s_console, &line, now);
            console_tad_attach(t, s);
            if (s == NULL && line.close != NULL && t->session == NULL && !t->close_wanted)
            {
                static const char full[] = "\r\nall sessions in use\r\n";
                (void)line.write(line.ctx, (const uint8_t *)full, sizeof full - 1);
                line.close(line.ctx, CONSOLE_CLOSE_EXIT);
            }
        }
        (void)console_tad_deliver(t, now);
        (void)console_tad_release(t);
        console_tad_pump(t, now);
    }
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
           "         [--route NAME=N]     a system CONNECT may name, and its node number;\n"
           "                              repeat it. --system NAME=--peer is always there\n"
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

/* --route NAME=N */
static bool parse_route(const char *v)
{
    const char *eq = strchr(v, '=');
    uint16_t n = 0;
    if (eq == NULL || !parse_u16(eq + 1, &n) || n == 0)
    {
        return false;
    }
    return route_add(v, (size_t)(eq - v), n);
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
/* size exemption: option table */
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
    else if (strcmp(k, "--route") == 0)
    {
        return parse_route(v);
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

/* The line: read what came; on a loss every session over it is dropped here. */
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
    for (int i = 0; i < HOST_PLACES; i++)
    {
        Place *p = &s_places[i];
        if (p->listing)
        {
            (void)xrout_walk_cancel(&p->walk); /* walk_done tells the user */
        }
        if (p->pending)
        {
            svc_hangup(NULL, i);
        }
        tad_asker_abort(&p->asker); /* the end reaches the console from place_service */
    }
    for (int k = 0; k < HOST_RESPONDERS; k++)
    {
        tad_responder_abort(&s_tad[k].r);
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
    cb.on_letter = on_letter;
    return xmsg_node_init(&s_node, &cfg, &ports, &cb);
}

static int console_setup(void)
{
    ConsoleConfig cfg;
    console_config_default(&cfg);
    cfg.mode = (s_mode == MODE_PROMPT) ? CONSOLE_MODE_PROMPT : CONSOLE_MODE_DIRECT;
    cfg.prompt = "TAD> ";
    snprintf(s_banner, sizeof s_banner, "libxmsg TAD bridge, node %u, linked to %s\r\n",
             (unsigned)s_own, s_system);
    cfg.banner = s_banner;
    cfg.direct_system = s_system;
    cfg.tad_user = s_iam_user; /* what TADA shows; the same name for every user, as before */
    cfg.escape_ms = s_esc_ms;
    cfg.store = s_store;
    cfg.rng = s_rng;
    cfg.users.get = users_get;
    cfg.users.remove = users_remove;
    cfg.services.resolve = svc_resolve;
    cfg.services.onward_open = svc_open;
    cfg.services.onward_write = svc_write;
    cfg.services.onward_escape = svc_escape;
    cfg.services.onward_hangup = svc_hangup;
    cfg.services.list_start = svc_list;
    cfg.services.list_cancel = svc_list_cancel;
    cfg.services.link_info = svc_link_info;
    tad_setup();
    return console_host_init(&s_console, &cfg, s_sessions, HOST_PLACES);
}

/* Wait for any socket, at most 10 ms. */
static void wait_sockets(Sock lsock, fd_set *rd)
{
    Sock maxs = lsock;
    FD_ZERO(rd);
    FD_SET(lsock, rd);
    if (s_line != SOCK_NONE)
    {
        FD_SET(s_line, rd);
        maxs = (s_line > maxs) ? s_line : maxs;
    }
    for (int i = 0; i < HOST_CLIENTS; i++)
    {
        if (s_clients[i].used)
        {
            FD_SET(s_clients[i].sock, rd);
            maxs = (s_clients[i].sock > maxs) ? s_clients[i].sock : maxs;
        }
    }
    struct timeval tv = {0, 10000};
    (void)select((int)maxs + 1, rd, NULL, NULL, &tv);
}

/* Send what each client has waiting; close the ones that are done. */
static void clients_flush(void)
{
    for (int i = 0; i < HOST_CLIENTS; i++)
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
                client_drop(c);
                continue;
            }
            memmove(c->out, c->out + n, c->out_len - (size_t)n);
            c->out_len -= (size_t)n;
        }
        if (c->closing && c->out_len == 0)
        {
            client_drop(c);
        }
    }
}

static void serve(Sock lsock, const char *host, const char *port)
{
    uint32_t next_dial = 0;
    for (;;)
    {
        uint32_t now = now_ms();
        uint8_t buf[2048];
        fd_set rd;

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
        wait_sockets(lsock, &rd);
        now = now_ms();
        if (s_line != SOCK_NONE && FD_ISSET(s_line, &rd))
        {
            line_read(buf, sizeof buf, now);
        }
        if (FD_ISSET(lsock, &rd))
        {
            client_accept(lsock, now);
        }
        for (int i = 0; i < HOST_CLIENTS; i++)
        {
            Client *c = &s_clients[i];
            if (c->used && FD_ISSET(c->sock, &rd))
            {
                int n = sock_recv(c->sock, buf, sizeof buf);
                if (n < 0)
                {
                    client_drop(c);
                }
                else if (!c->closing)
                {
                    client_rx(c, buf, (size_t)n, now);
                }
            }
        }
        for (int i = 0; i < HOST_PLACES; i++)
        {
            place_service(i, now);
        }
        tad_service(now);
        console_host_poll(&s_console, now);
        xmsg_node_tick(&s_node, now);
        clients_flush();
    }
}

/* Prompt mode needs a users file; make one with the default admin when there is none. */
static int users_prepare(void)
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
            return -1;
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
    return 0;
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
        (size_t)(colon - a.hdlc) >= sizeof host || strlen(colon + 1) >= sizeof port ||
        !route_add(s_system, strlen(s_system), s_peer))
    {
        usage();
        return 1;
    }
    memcpy(host, a.hdlc, (size_t)(colon - a.hdlc));
    host[colon - a.hdlc] = '\0';
    snprintf(port, sizeof port, "%s", colon + 1);
    snprintf(s_line_name, sizeof s_line_name, "HDLC over TCP %s:%s", host, port);
    if (s_mode == MODE_PROMPT && users_prepare() != 0)
    {
        return 1;
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
    if (node_setup(&store) != XMSG_OK || console_setup() != 0)
    {
        printf("set-up failed\n");
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
    for (int i = 0; i < s_route_count; i++)
    {
        printf("system %s is node %u\n", s_routes[i].name, (unsigned)s_routes[i].number);
    }
    serve(lsock, host, port);
    return 0;
}
