# libxmsg

[![build](https://github.com/RetroCoreLabs/libxmsg/actions/workflows/build.yml/badge.svg)](https://github.com/RetroCoreLabs/libxmsg/actions/workflows/build.yml)
[![release](https://img.shields.io/github/v/release/RetroCoreLabs/libxmsg)](https://github.com/RetroCoreLabs/libxmsg/releases)
[![licence](https://img.shields.io/github/license/RetroCoreLabs/libxmsg)](LICENSE)
[![downloads](https://img.shields.io/github/downloads/RetroCoreLabs/libxmsg/total)](https://github.com/RetroCoreLabs/libxmsg/releases)

**libxmsg is a small C library that lets a microcontroller or a PC talk to a Norsk Data SINTRAN III
machine over XMSG: log in to it as a terminal, and (optionally) move files to and from it.**

[What you can use it for](#what-you-can-use-it-for) · [Status](#status) · [Quick start](#quick-start) · [Building](#building-from-source) · [About XMSG](#about-xmsg) · [Documentation](#documentation)

```c
#include "libxmsg/xmsg.h"

/* Feed the bytes you receive, call tick with the time, read the callbacks. No threads, no sockets,
 * no allocation after start-up: the library runs the same way on an ESP32, a Pico and a PC. */
xmsg_node_feed(&node, bytes, length, now_ms);
xmsg_node_tick(&node, now_ms);
```

## How this code was written

Some of the code in this repository was written with the help of AI, mostly Claude (Anthropic).
Ronny Hansen directs the work, tests it against real hardware and emulators, and is responsible for
what is in the repository. We say so plainly so you know what you are using.

## Why it exists

A Norsk Data machine from the 1980s talks to other machines over XMSG, its own messaging system,
carried on a synchronous HDLC line. Everything rides on it: remote terminal sessions, file transfer,
the name service. So reaching a SINTRAN machine for almost any purpose means speaking XMSG.

The protocol is already implemented and proven against real machines in C# (the RetroIO packages),
but C# does not fit on a 20-dollar board that sits next to the machine and offers a telnet port. This
library is that protocol, rewritten in portable C11 for small targets, with every byte pinned to the
captures and the machine-readable protocol registry the C# work produced.

It has no sockets, no serial port, no file system and no clock of its own. The program that uses it
supplies those through small tables of function pointers, which is what lets one set of source files
serve an Arduino sketch, a Raspberry Pi Pico program and a PC test tool, and lets a unit test be the
transport.

## What you can use it for

- **A terminal server for a SINTRAN machine** - an ESP32 listens on TCP; each telnet client gets its
  own login session on the ND machine (the TAD protocol).
- **Bringing up and holding an XMSG link** - LAPB in the ND dialect, with the persistent datagram
  sequence SINTRAN expects to continue across your restarts.
- **COSMOS file access, optional** (`libcosmos`): fetch a file, a listing or file statistics from
  the machine, or offer it a remote directory it can copy files to and from. The storage behind that
  directory is yours to supply; the library never touches a file system.
- **An FTP front end, optional**: a socket-free FTP command core that maps LIST, RETR, STOR, DELE
  and SIZE onto the file-access operations, so any PC file manager can reach the ND machine.
- **Developing against an emulator**: everything runs first against nd100x over a TCP port.

## Status

Version 0.1.0 - the link, the datagrams and the terminal session are written, tested against
captured traffic, and run against a live SINTRAN III K machine (RetroCore's ND-100 emulator): link
up and held, login, LIST-FILES, LOGOUT and hang-up. The integration tests skip when no machine is
reachable.

- **Framing and link:** the RFC 1662 byte framer with FCS-16, and LAPB in the ND dialect (node
  number in every U and S frame, balanced bring-up, the 0x89 address for odd-length frames).
- **Datagrams:** the SINTRAN header and checksum, the sub-header, fragmentation, one Flags1 count
  per peer kept across restarts, the reachability handshake, and an ACK for every data frame.
- **Terminal session (TAD):** the CONNECT-TO ladder from the connect letter to the hang-up.
  Replayed against a complete session captured between two real SINTRAN
  machines, every frame the library sends is byte-identical to the one the real asker sent.
- **Telnet tool:** `examples/host_tad_telnet` gives each telnet client its own session.
- **Still to come:** several sessions on microcontrollers, then COSMOS file access and the FTP
  bridge.

## Prerequisites

| # | Requirement | Needed for | How to check |
|---|---|---|---|
| P1 | CMake 3.13 or newer | Building the library and tests | `cmake --version` |
| P2 | GCC or Clang with C11 (MinGW GCC on Windows) | Everything | `gcc --version` |
| P3 | Python 3 | Regenerating the protocol headers only | `python --version` |
| P4 | nd100x with `--hdlc=1:PORT`, SINTRAN with XMSG started | The integration tests, which otherwise skip | the emulator console |

## Quick start

```bash
git clone https://github.com/RetroCoreLabs/libxmsg.git
cd libxmsg
cmake -S src -B build -DXMSG_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

To telnet to a SINTRAN machine, start nd100x with `--hdlc=1:1362`, define a link there for node
19999, and run the tool. Then `telnet localhost 2300`:

```bash
build/examples/host_tad_telnet/host_tad_telnet --hdlc 127.0.0.1:1362 --node 19999 \
    --peer 100 --system D100 --listen 2300
```

The tool has two modes, chosen with `--mode`:

| Mode | What a telnet client gets | When SINTRAN logs out |
|---|---|---|
| `direct` (default) | a TAD session on `--system` at once | the session is hung up and the telnet connection closed |
| `prompt` | `login:` / `password:`, then the tool's `TAD>` command line | the session is hung up and the user is back at `TAD>` |

At `TAD>`: `CONNECT <system>` (for example `CONNECT D102`), `LIST` (the routing table of the host we are linked to), `NAMES`
(the system names and named ports the host knows, as LIST-NAMES shows them), `WHO`
(the clients on the tool), `INFO` (the link: nodes, state, counters, uptime), `HELP`, and
`EXIT`/`LOGOUT`/`QUIT`, and `PASSWORD` (change your own password: old, new, again). Admins also have
`USERS` (the users, level and change flag), `ADDUSER name [admin]` (asks the password twice; the new
user must change it at the first login), `DELUSER name` (never the last admin) and `RESETPW name`
(a new password, asked twice, that the user must change at the next login); other users get
"not allowed". In both modes a dropped telnet connection hangs the TAD session up at once.

Each user has a level: **admin** (may add and delete users, reset any password, change settings)
or **user** (may use ND sessions and change their own password). A user marked "must change" is
asked for a new password twice right after the login, and the users file is written before `TAD>`
shows; an empty password, the old password again, or two entries that differ are refused.

Other switches:

| Switch | Default | Meaning |
|---|---|---|
| `--users FILE` | `host_tad_telnet.users` | the users for prompt mode; when the file is missing or holds no user, prompt mode creates it with the default admin (name `admin`, password `admin`, must change) and prints a warning |
| `--esc-ms N` | 50 | an ESC with no other byte within N ms is the escape key (the host breaks the running program); 0 = ESC is always text |
| `--term-type N` | 0 | the terminal type sent to the host, decimal |
| `--user NAME` | `TELNET` | the user name TADA shows for the session, at most 16 characters |
| `--raw-keys` | off | typed bytes go without the parity bit |

Users are kept with a PBKDF2-HMAC-SHA256 hash (random 16-byte salt, 100000 iterations), one line
each: `name:pbkdf2-sha256:<iterations>:<salt hex>:<hash hex>:<flags>`, where `<flags>` is `admin`
or `user`, optionally followed by `,change` (for example `admin,change`: must set a new password at
the next login). A line without the flags field, from an older file, reads as `user` with no change
needed; the tool always writes all six fields. Lines starting with `#` are comments. Manage them
without a running tool:

```bash
host_tad_telnet --add-user NAME [--admin] [--users FILE]  # asks the password twice, echo off; level user unless --admin
host_tad_telnet --del-user NAME [--users FILE]            # refuses to delete the last admin
host_tad_telnet --list-users [--users FILE]               # name, level, and "must change password"
host_tad_telnet --reset-password NAME [--users FILE]      # new password twice; must change at next login
```

The password hash runs through a replaceable function (`auth_hash_set_port()` in
`src/libxmsg/auth/auth_hash.h`), so a board can use a faster implementation, such as mbedTLS on
the ESP32 SHA hardware; without one the library's own PBKDF2 is used. The header describes how.

## Building from source

The CMake entry point is `src/CMakeLists.txt`; the file at the repository root only forwards to it.

| Option | Default | Meaning |
|---|---|---|
| `XMSG_WITH_COSMOS` | OFF | Also build `libcosmos`, the COSMOS file-access client and server |
| `XMSG_WITH_FTP` | OFF | Also build the FTP command core (needs `XMSG_WITH_COSMOS`) |
| `LIBXMSG_WITH_XMSG` | `ON` | `OFF` builds only libhdlc (`hdlc::hdlc`), for a project that needs the HDLC code alone |
| `XMSG_BUILD_TESTS` | ON when top level | The CTest unit tests |
| `XMSG_WERROR` | OFF | Every warning is an error (CI turns this on) |
| `XMSG_MAX_SESSIONS`, `XMSG_MAX_PEERS`, `XMSG_LAPB_N1`, ... | see `src/libxmsg/xmsg_config.h` | Compile-time sizes; they decide the whole RAM footprint |

**From another CMake project:** `add_subdirectory(external/libxmsg)` and
`target_link_libraries(app PRIVATE xmsg::xmsg)`.

**Arduino:** the repository is an Arduino library (`library.properties` at the root, sources under
`src/`). Add it to the IDE from the repository URL or from a release zip.

**Raspberry Pi Pico:** a Pico SDK project adds the repository with `add_subdirectory` and links
`xmsg::xmsg`; the examples under `examples/` show the wiring.

## About XMSG

XMSG is the message-passing system of SINTRAN III, the operating system of the Norsk Data ND-100 and
ND-500 minicomputers that ran Norwegian universities, industry and defence through the 1980s. Tasks
on one machine send messages to named ports on another; the terminal-access service (TAD), the
COSMOS file servers and the routing service are all just tasks holding ports. Between two machines
the messages travel as datagrams over LAPB, a thin point-to-point link layer on a synchronous HDLC
line. The machines are gone from service but not from existence: emulators run their disks, and a
few real ones still power up, and this library is how a modern device joins their network.

## Documentation

- The protocol registry this library is generated from: `extras/protocols/` (a vendored copy; the
  original lives in the RetroIO and NDInsight repositories).
- The C# reference implementation: the `RetroIO.Protocols.Xmsg` packages in the RetroIO repository.
- The nd100x emulator, whose `--hdlc` option is the test transport: the nd100x repository.
- libhdlc (`src/libhdlc`, moved in from RetroCoreLabs/libhdlc on 2026-11-10 with its history): ALL
  the HDLC code - bit-level framing, CRC-16, the RFC 1662 byte framing, ring buffers. It is its own
  CMake target, `hdlc::hdlc`, and its own header, `libhdlc.h`; its notes are in
  `extras/docs/libhdlc/`.

## Contributing

See [CONTRIBUTING.md](.github/CONTRIBUTING.md). Security problems go through
[SECURITY.md](.github/SECURITY.md), never a public issue.

## Acknowledgements

The wire formats come from captures of real ND machines and from reading SINTRAN's own sources and
manuals, work recorded in the NDInsight repository. The HDLC bit-stuffing logic on the synchronous
line side of the companion gateways comes from Mattis Lind's SDLCBridge.

## Licence

MIT. See [LICENSE](LICENSE).
