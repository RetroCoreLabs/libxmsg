/*
 * xmsg_config.h - compile-time sizes and feature switches for libxmsg.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Every value here can be overridden from the build: CMake writes them as -D definitions on the
 * xmsg target, an Arduino sketch can set them in a build_opt.h, and a hand build passes -D flags.
 * The #ifndef guards mean this file only supplies the default when nobody else did.
 *
 * Nothing in the library allocates after start-up, so these numbers decide the whole RAM footprint:
 * see the memory table in the repository README.
 */
#ifndef XMSG_CONFIG_H
#define XMSG_CONFIG_H

/* Sessions (TAD terminal sessions, file-access conversations, ...) one node can hold at once. */
#ifndef XMSG_MAX_SESSIONS
#define XMSG_MAX_SESSIONS 4
#endif

/* Remote nodes we keep an outgoing message counter (Flags1) for. One entry per peer machine. */
#ifndef XMSG_MAX_PEERS
#define XMSG_MAX_PEERS 4
#endif

/* Fragmented messages held while their continuation arrives. Measured peak on real traffic is 1. */
#ifndef XMSG_REASSEMBLY_SLOTS
#define XMSG_REASSEMBLY_SLOTS 1
#endif

/*
 * Largest LAPB information field in bytes. 622 is what the C# reference and SINTRAN's own frame
 * size (X3FSZ = 312 words = 624 bytes) agree on. A TAD-only build on a line that only carries
 * 128-byte frames may lower it; file-access content frames are then refused with FRMR, loudly.
 */
#ifndef XMSG_LAPB_N1
#define XMSG_LAPB_N1 622
#endif

/* Highest log level compiled in: 0 ERROR, 1 WARN, 2 INFO, 3 DEBUG, 4 TRACE. Lower it on a small
 * target so the format strings of the stripped levels never reach flash. */
#ifndef XMSG_LOG_MAX_LEVEL
#define XMSG_LOG_MAX_LEVEL 4
#endif

/* Library version, also in library.properties and src/CMakeLists.txt - keep the three in step. */
#define XMSG_VERSION_MAJOR  0
#define XMSG_VERSION_MINOR  1
#define XMSG_VERSION_PATCH  0
#define XMSG_VERSION_STRING "0.1.0"

#endif /* XMSG_CONFIG_H */
