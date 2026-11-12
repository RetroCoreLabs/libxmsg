# libxmsg - known defects

Each entry names the case and its cause. A defect stays here until it is fixed at the root.

## B1 - TAD break strategies 2 to 6, 8, and a mixed strategy 7 table are not implemented

**Where:** `src/libxmsg/tad/tad_asker.c`, `take_break_setting()` and `is_break_char()`.

**What:** the host's BMMX message (TAD opcode 0x04: strategy byte, BRKMAX word, and for
strategy 7 a 16-byte break table) tells the asker which typed character ends a piece of input.
Implemented: 0 (every character), 1 (control characters and DEL, SINTRAN's default), negative
(none), 9 (change BRKMAX only), BRKMAX as the most bytes per letter for strategies 3 and up, and
a strategy 7 table that is all ones (every character) or all zeros (none). For 2 (the MAC
assembler's table), 3 to 6 (system defined), 8 (last user table) and a strategy 7 table with
some bits set, the asker logs an ERROR, counts `stats.unknown_break_strategy`, and cuts input
only at BRKMAX.

**Why it is open:** the character sets of strategies 2 to 6, and the bit order of the 16-byte
table, are not known. The SINTRAN Monitor Calls manual (MON BRKM, InputString) names the
strategies but not their tables. What to do: carve the tables and the bit order from the
SINTRAN TAD code (BDBREA in 20-COS-TAD-POF-CODE.NPL and the terminal driver's break tables), or
capture a host program that sets a mixed table.

**Measured so far (2026-11-10, RetroServer hub and XMSGGW's line, D100):** the command prompt
uses 1; S3-CONFIG's screen sets 7 with BRKMAX 1 and a table of all ones, and after the first
typed byte D100 sets 0.

# libcosmos

## C1 - the character after the apostrophe in a read open is not understood

**Where:** `src/libcosmos/fa/fa_requests.c`, `fa_build_open_read()`.

**What:** a read open names the file as `<spec>'` and one more character. RetroIO's reader sends
`.` (from a capture of `BIGPSH3:TXT'.`) and has read files live with it. The D100 client in
`capture-read.txt` sent `PATCH-FILE:OUT'T`, and in `capture-open-error.txt` `NO-SUCH-FILE:OUT'U`.
libcosmos sends `.`. The unit test allows exactly this byte to differ from the capture and
checks that ours is `.`.

**Why it is open:** what the character means, and whether a server ever looks at it, is not
known. What to do: find where the SINTRAN FA client (COSMOS) builds the open request, or capture
the same file opened twice from two programs.

## C2 - short-ack counter past 127 is not verified

**Where:** `src/libcosmos/fa/fa_client.c`, `enqueue_ack()`.

**What:** the client's short acks count 1, 2, 3, ... in the low byte of the counter field, bit 7
clear. A read of N blocks sends about 3N + 6 of them, so a file over about 80 KB (40 blocks)
reaches 0x80. The captures stop at 0x21. libcosmos counts on as an 8-bit number (as RetroIO's
`FaClientConversation.NextAckCounter` does), so 0x80 and up go out with bit 7 set and the count
wraps from 0xFF to 0x00.

**Why it is open:** no capture of a transfer that long. What to do: capture a real client
reading a file of 100 KB or more and read the counter of its 128th short ack.

## C3 - a write whose source fails leaves the file open on the server

**Where:** `src/libcosmos/fa/fa_client.c`, `build_data()` and `client_poll_tx()`.

**What:** `fa_client_write` asks the application for each 1024-byte half of a block just
before sending it. When the `source` callback gives less than it was asked for, the write ends
at once with XMSG_EIO. By then the write request for that block has gone out, so the server is
waiting for the block's data. The client sends no close, no release and no session finished,
so the file stays open on the server and its seat stays taken. How long the server keeps them
is not known. RetroIO measured on D100 on 2026-08-17 that a write which stopped part way left
the file open until the file server was restarted (`FaClientWriteSession._fileOpenOnPeer`).

**Why it is open:** the block that was announced cannot be finished without its bytes, and
sending zeros instead would write a wrong file. What to do: ask the source for both halves of
a block before its write request goes out (the client then needs a 2048-byte buffer, or a
check-only call on the source), so a failure is found while no block is announced and the
normal teardown (close, release, session finished) can run.

# libhdlc (moved in from RetroCoreLabs/libhdlc, 2026-11-10)


### L-PICO - ring_buffer has no lock on the Raspberry Pi Pico

`ring_buffer.h` stops a Pico build with `#error`; `ring_buffer.c` compiles to nothing there, so
the rest of the library still builds.

The Pico uses **`spsc_ring`** instead (Ronny's decision, 2026-11-09): lock-free, exactly one
writer and one reader, safe from an interrupt. It builds for the Cortex-M0+ with no helper
library and with the memory barriers in place (9 `dmb` in the object file, checked with the
Pico SDK's arm-none-eabi-gcc).

Why ring_buffer itself gets no Pico lock: RC422Dev builds FreeRTOS with
`configUSE_MUTEXES = 0`, so the ESP32's FreeRTOS mutex does not exist there, and RC422Dev only
ever has one writer and one reader per ring.

Still open: spsc_ring has run on Windows and Linux with real threads, not yet on a Pico.
RC422Dev is to confirm on the board.
