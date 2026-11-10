# libhdlc

> This was the README of the separate libhdlc repository. Since 2026-11-10 libhdlc lives in
> libxmsg under `src/libhdlc`; build it with libxmsg's CMake (`-DLIBXMSG_WITH_XMSG=OFF` for libhdlc
> alone) and its tests are in `extras/tests/libhdlc`. Paths and badges below are from the old repository.

[![build](https://github.com/RetroCoreLabs/libhdlc/actions/workflows/build.yml/badge.svg)](https://github.com/RetroCoreLabs/libhdlc/actions/workflows/build.yml)
[![licence: MIT](https://img.shields.io/badge/licence-MIT-blue.svg)](LICENSE)
[![C11](https://img.shields.io/badge/C-11-informational.svg)](#building-and-testing)

[About HDLC](#about-hdlc) | [Status](#status) | [Platforms](#platforms) | [Using it](#using-it) | [Building and testing](#building-and-testing) | [Control frames](extras/docs/control-frames.md)

HDLC framing in portable C11, for gateways that put a synchronous HDLC/SDLC line on a PC. It is
shared by HDLCGW (ESP32-S2) and RC422Dev (Raspberry Pi Pico), and builds on Linux and Windows for
host tools and unit tests.

| Part | Header | What it does |
|---|---|---|
| Bit-level HDLC | `libhdlc/hdlc.h` | Bit stuffing, opening/closing flags, aborts, CRC-16/X.25 frame check, control-field decode. The encoder gives packed line bytes (for SPI) or groups of 8-10 bits (for a Pico PIO). The decoder takes line bytes, handles a whole byte at once when it holds no stuffing, flag or abort, and reports each frame as OK / CRC error / odd bit count / too long, with counters. Frame bytes go to a callback one at a time, or straight into a caller's buffer with one callback per frame (`hdlc_decoder_set_buffer`). |
| Byte-stream framing | `libhdlc/byte_frame.h` | RFC 1662 byte stuffing for frames on a byte stream: USB serial, TCP, the nd100x bridge. A full engine (caller-owned buffer, FCS check on or off, any escape set, a resumable encoder for non-blocking transports; used by libxmsg) and a short form for gateways. The frame check functions are in `hdlc.h`. |
| Control frames | `libhdlc/control_frame.h` | Messages between the PC and the gateway firmware on the same channel as line frames: commands, replies, log, statistics, events. Described in full in [extras/docs/control-frames.md](extras/docs/control-frames.md). |
| Ring buffer | `libhdlc/ring_buffer.h` | A byte ring guarded by a mutex, for passing bytes between tasks or threads, any number of writers. FreeRTOS on ESP32, SRWLOCK on Windows, pthread elsewhere. Not on the Pico ([BUGS.md](BUGS.md) L-PICO). |
| Lock-free ring | `libhdlc/spsc_ring.h` | A byte ring for exactly ONE writer and ONE reader, no lock, safe from an interrupt. The ring for the Pico. Two writers at once corrupt it. |

`#include "libhdlc/libhdlc.h"` (or `<libhdlc.h>` in an Arduino sketch) pulls in all of it.

## About HDLC

HDLC (High-Level Data Link Control, ISO 13239) and its IBM ancestor SDLC send frames over a
synchronous serial line: a clock wire plus a data wire, bits flowing continuously. A frame starts
and ends with the flag `01111110`. Inside a frame the sender inserts a 0 after every five 1s in a
row (bit stuffing), so the flag pattern can never appear in the data, and seven 1s in a row abort
a frame. The last two bytes are a CRC-16 frame check. Norsk Data machines, X.25 equipment and IBM
terminals talk this way; a gateway built on this library lets a PC take part.

## Status

Version 0.1.0, first commit 2026-11-09. Measured that day:

- unit tests pass on Windows (gcc, w64devkit) and Linux (gcc, Ubuntu): 7 test programs, over
  20000 checks;
- HDLCGW (ESP32-S2) built against it passed the full two-board sweep against RC422Dev: 376 cases,
  0 failures, 19200 to 983000 bit/s, frames of 10 to 626 bytes, no frame lost or bad;
- every file compiles for the Pico's Cortex-M0+; it has not yet run on a Pico.

## Platforms

| Platform | Status |
|---|---|
| Windows (gcc, w64devkit) | unit tests pass |
| Linux (gcc, Ubuntu under WSL) | unit tests pass |
| ESP32 (Arduino core 3.3.12) | used by HDLCGW |
| Raspberry Pi Pico | every file compiles for the Cortex-M0+ (Pico SDK gcc); use spsc_ring, not ring_buffer. Not yet run on a board. |

## Using it

**Arduino IDE / arduino-cli:** the repository is an Arduino library (`library.properties`, code
under `src/`). Put it, or a link to it, in the sketchbook's `libraries` folder, then
`#include <libhdlc.h>` once in the .ino: arduino-cli finds a library only by a header at the top
of its `src/` folder. After that any file can include `"libhdlc/<name>.h"`.

**CMake:**

```cmake
add_subdirectory(path/to/libhdlc/src)
target_link_libraries(app PRIVATE hdlc::hdlc)
```

## Building and testing

```sh
cmake -S src -B build
cmake --build build
ctest --test-dir build
```

On Windows with w64devkit, add `-G "MinGW Makefiles"` to the first line.

The tests are plain C, one program per module, under `extras/tests`. Besides fixed vectors
(computed separately in Python, never by this library), they include a second, deliberately simple
bit-by-bit receiver written straight from the HDLC rules. The library must report exactly the same
bytes and frame endings on 2000 random lines full of flags, stuffing and aborts. That is what shows
the whole-byte fast path changes nothing.

## Where the code came from

- The bit-level logic follows Mattis Lind's SDLCBridge (`MattisLind/alfaskop_emu`,
  `Utils/SDLCBridge`), proven against a real IBM SDLC terminal and an Intel 8274 board. `hdlc.h`
  lists the two details kept from it on purpose.
- The whole-byte receive table, the closing-flag alignment check and the bit-group encoder come
  from RC422Dev, measured on the Pico at 983000 bit/s.
- The byte-stream engine and its tests come from libxmsg (2026-11-09), which mirrors RetroIO.Hdlc.
- control_frame and ring_buffer come from HDLCGW.

## Licence

MIT, see [LICENSE](LICENSE).
