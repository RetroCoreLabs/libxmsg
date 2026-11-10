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
