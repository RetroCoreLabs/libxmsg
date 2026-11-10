/*
 * xmsg.h - umbrella header for libxmsg: SINTRAN III XMSG over LAPB over HDLC.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Include this one file to get the whole public API. The headers it pulls in are each
 * self-contained and may be included on their own.
 *
 * The layers, bottom up:
 *   hdlc/     byte framing (0x7E flags, 0x7D escapes, FCS-16) for TCP bridges and serial gateways
 *   lapb/     the ND dialect of LAPB: link bring-up, numbered frames, retransmission
 *   sintran/  the 14-byte SINTRAN datagram header and its checksum
 *   xmsg/     datagrams, acknowledgements, fragments, the per-peer sequence, the node
 *   xrout/    the letter that reaches a named service on the far machine, routing info, names
 *   tad/      a terminal session to a SINTRAN host
 *   gen/      constants and accessors generated from the protocol registry - never edited by hand
 */
#ifndef XMSG_H
#define XMSG_H

#include "libxmsg/xmsg_config.h"
#include "libxmsg/xmsg_endian.h"
#include "libxmsg/xmsg_errors.h"
#include "libxmsg/xmsg_ports.h"

#include "libhdlc/hdlc.h"
#include "libhdlc/byte_frame.h"
#include "libxmsg/lapb/lapb.h"
#include "libxmsg/lapb/lapb_frame.h"
#include "libxmsg/log/xmsg_log.h"
#include "libxmsg/sintran/sintran_hdr.h"
#include "libxmsg/xmsg/xmsg_dg.h"
#include "libxmsg/xmsg/xmsg_flags1.h"
#include "libxmsg/xmsg/xmsg_frag.h"
#include "libxmsg/xmsg/xmsg_node.h"
#include "libxmsg/xmsg/xmsg_portword.h"
#include "libxmsg/xmsg/xmsg_session.h"
#include "libxmsg/xmsg/xmsg_subhdr.h"
#include "libxmsg/xrout/xrout_letter.h"
#include "libxmsg/xrout/xrout_route.h"
#include "libxmsg/xrout/xrout_walk.h"
#include "libxmsg/xrout/xrout_name.h"
#include "libxmsg/xrout/xrout_names_walk.h"
#include "libxmsg/tad/tad_codec.h"
#include "libxmsg/tad/tad_asker.h"
#include "libxmsg/tad/tad_responder.h"
#include "libxmsg/term/term_escape.h"
#include "libxmsg/term/term_keyseq.h"
#include "libxmsg/term/term_line.h"
#include "libxmsg/term/term_report.h"
#include "libxmsg/term/term_telnet.h"
#include "libxmsg/auth/auth_hash.h"
#include "libxmsg/auth/auth_user.h"
#include "libxmsg/auth/auth_login.h"
#include "libxmsg/console/console.h"
#include "libxmsg/console/console_tad.h"

#include "libxmsg/gen/sintran_wire.h"
#include "libxmsg/gen/tad_wire.h"
#include "libxmsg/gen/xrout_services.h"

#endif /* XMSG_H */
