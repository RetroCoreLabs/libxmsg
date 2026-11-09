# libhdlc

HDLC framing in portable C11, for gateways that put a synchronous HDLC/SDLC line on a PC. It is
shared by HDLCGW (ESP32-S2) and RC422Dev (Raspberry Pi Pico), and builds on Linux and Windows for
host tools and unit tests.

| Part | Header | What it does |
|---|---|---|
| Bit-level HDLC | `libhdlc/hdlc.h` | Bit stuffing, opening/closing flags, aborts, CRC-16/X.25 frame check, control-field decode. The encoder gives packed line bytes (for SPI) or groups of 8-10 bits (for a Pico PIO). The decoder takes line bytes, handles a whole byte at once when it holds no stuffing, flag or abort, and reports each frame as OK / CRC error / odd bit count / too long, with counters. |
| Host framing | `libhdlc/byte_frame.h` | RFC 1662 byte stuffing between the gateway and the PC over USB serial or TCP. |
| Control frames | `libhdlc/control_frame.h` | Messages between the PC and the gateway firmware on the same channel as line frames: commands, replies, log, statistics, events. Described in full in [extras/docs/control-frames.md](extras/docs/control-frames.md). |
| Ring buffer | `libhdlc/ring_buffer.h` | A byte ring guarded by a mutex, for passing bytes between tasks or threads, any number of writers. FreeRTOS on ESP32, SRWLOCK on Windows, pthread elsewhere. Not on the Pico ([BUGS.md](BUGS.md) L-PICO). |
| Lock-free ring | `libhdlc/spsc_ring.h` | A byte ring for exactly ONE writer and ONE reader, no lock, safe from an interrupt. The ring for the Pico. Two writers at once corrupt it. |

`#include "libhdlc/libhdlc.h"` (or `<libhdlc.h>` in an Arduino sketch) pulls in all of it.

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
- byte_frame, control_frame and ring_buffer come from HDLCGW.

## Licence

MIT, see [LICENSE](LICENSE).
