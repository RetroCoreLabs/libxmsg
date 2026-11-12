/*
 * fa_open_error_d100_d102.h - every FA message of a refused open, in order.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Made from RetroIO DOC/captures/FA-READ-WRITE-2026-08-04/capture-open-error.txt, records
 * 02:37:41.865 to 02:38:15.882: a D100 client asks D102 for NO-SUCH-FILE:OUT and is refused
 * with SINTRAN error 46.
 * Each entry is one XMSG message body (the bytes after the 28-byte header); a fragmented
 * data message is joined from its two frames. C = sent by the client (node 100), S = sent
 * by the file server (node 102). The capture time is in the comment.
 */
#ifndef FA_OPEN_ERROR_D100_D102_H
#define FA_OPEN_ERROR_D100_D102_H

#include "vectors/fa_vector_event.h"

static const FaVectorEvent k_fa_open_error_events[] = {
    /* 02:37:41.865 */ {'C',
                        "1B410012FF0A2A46412D534552564552FE044431303207E2000000026400A200FF00"},
    /* 02:37:41.892 */ {'S', "07D2000200456400"},
    /* 02:37:41.939 */
    {'C', "07F0004580000001920002920001F20001A207D0F200028C06920001920001F20003BD42414B303420205359"
          "5354454DF200048C38B01053595354454D27000000000000000000E180B01000000000000000000000000000"
          "000000B01000000000000000000000000000000000F200FF"},
    /* 02:37:42.065 */ {'S', "07A200020100922A"},
    /* 02:37:42.175 */ {'S', "07F00002800090BB920002920001F200FFA2"},
    /* 02:37:42.228 */ {'C', "07A2004501008485"},
    /* 02:37:42.238 */
    {'C', "07F000458100D761920005920002F20002B0124E4F2D535543482D46494C453A4F55542755F200FF"},
    /* 02:37:42.286 */ {'S', "07A200020200922A"},
    /* 02:37:42.349 */ {'S', "07F00002810090BB920005920002F20001A2002EF200FFA4"},
    /* 02:37:42.368 */ {'C', "07A2004502008485"},
    /* 02:38:15.706 */ {'C', "07F000458200D761920003920003F200FF00"},
    /* 02:38:15.732 */ {'S', "07A200020300922A"},
    /* 02:38:15.744 */ {'S', "07F00002820090BB920003920003F200FFA2"},
    /* 02:38:15.780 */ {'C', "07A2004503008485"},
    /* 02:38:15.795 */ {'C', "07820045000280000000"},
    /* 02:38:15.882 */ {'S', "07C0000200450000"},
};

#endif /* FA_OPEN_ERROR_D100_D102_H */
