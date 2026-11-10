# libxmsg - known defects

Each entry names the case and its cause. A defect stays here until it is fixed at the root.

## B1 - TAD break strategies 2 to 8 are not implemented

**Where:** `src/libxmsg/tad/tad_asker.c`, `take_break_setting()` and `is_break_char()`.

**What:** the host's BMMX message (TAD opcode 0x04: strategy byte, BRKMAX word, and for
strategy 7 a 16-byte break table) tells the asker which typed character ends a piece of input.
Only strategies 0 (every character), 1 (control characters and DEL, SINTRAN's default) and the
negative values (no character) are implemented. For 2 (the MAC assembler's table), 3 to 6
(system defined), 7 (user table) and 8 (last user table) the asker logs
an ERROR, counts `stats.unknown_break_strategy`, and does not cut input at all. Strategy 9 (change BRKMAX only) keeps the strategy in force;
BRKMAX itself is recorded but not used, since strategies 0 and 1 ignore it.

**Why it is open:** the character sets of strategies 2 to 6, and the bit order of the 16-byte
table in a strategy 7 BMMX, are not known. The SINTRAN Monitor Calls manual (MON BRKM,
InputString) names the strategies but not their tables. What to do: carve the tables from the
SINTRAN TAD code (BDBREA in 20-COS-TAD-POF-CODE.NPL and the terminal driver's break tables), or
capture a host program that sets one.

**Measured so far (2026-11-10, RetroServer hub, D102 CONNECT-TO D100):** S3-CONFIG sets
strategy 0; the command prompt uses 1. No other strategy has been seen on the wire.
