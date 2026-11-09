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

## Building from source

The CMake entry point is `src/CMakeLists.txt`; the file at the repository root only forwards to it.

| Option | Default | Meaning |
|---|---|---|
| `XMSG_WITH_COSMOS` | OFF | Also build `libcosmos`, the COSMOS file-access client and server |
| `XMSG_WITH_FTP` | OFF | Also build the FTP command core (needs `XMSG_WITH_COSMOS`) |
| `XMSG_WITH_ASYNC_FRAMER` | ON | Compile the byte framer; turn off when the line hardware frames |
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

## Contributing

See [CONTRIBUTING.md](.github/CONTRIBUTING.md). Security problems go through
[SECURITY.md](.github/SECURITY.md), never a public issue.

## Acknowledgements

The wire formats come from captures of real ND machines and from reading SINTRAN's own sources and
manuals, work recorded in the NDInsight repository. The HDLC bit-stuffing logic on the synchronous
line side of the companion gateways comes from Mattis Lind's SDLCBridge.

## Licence

MIT. See [LICENSE](LICENSE).
