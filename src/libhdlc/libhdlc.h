/*
 * libhdlc.h - umbrella header for libhdlc. Include this one file to get the whole API; each
 * header it pulls in is self-contained and may be included on its own.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 *   hdlc.h           bit-level HDLC/SDLC: bit stuffing, flags, aborts, CRC-16/X.25,
 *                    control-field decode (synchronous line, clocked through SPI or a PIO)
 *   byte_frame.h     RFC 1662 byte-stuffed frames between a gateway and its host PC
 *   control_frame.h  messages between the host PC and the gateway firmware itself, on the
 *                    same byte-stuffed channel (see extras/docs/control-frames.md)
 *   ring_buffer.h    a byte ring guarded by a mutex, for passing bytes between tasks
 *   hot_path.h       puts per-byte code and tables in internal RAM on the ESP32
 */
#ifndef LIBHDLC_H
#define LIBHDLC_H

#include "libhdlc/byte_frame.h"
#include "libhdlc/control_frame.h"
#include "libhdlc/hdlc.h"
#include "libhdlc/hot_path.h"
#include "libhdlc/ring_buffer.h"
#include "libhdlc/spsc_ring.h"

#endif /* LIBHDLC_H */
