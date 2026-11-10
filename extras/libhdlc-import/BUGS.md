# Open defects and limits

## L-PICO - ring_buffer has no lock on the Raspberry Pi Pico

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
