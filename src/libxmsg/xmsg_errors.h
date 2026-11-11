/*
 * xmsg_errors.h - the return codes every libxmsg and libcosmos function uses.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * House rule 7.2: an action returns 0 on success and a negative value on failure. The handful of
 * values here cover every reason a call in this library can refuse; a function's header says
 * which of them it can return. Protocol-level errors that come from the far machine (XMSG error
 * codes, SINTRAN file errors) are not these: they are data, reported through callbacks.
 */
#ifndef XMSG_ERRORS_H
#define XMSG_ERRORS_H

#ifdef __cplusplus
extern "C"
{
#endif

#define XMSG_OK        0    /**< done */
#define XMSG_EINVAL    (-1) /**< a NULL pointer, a zero length, a value out of range */
#define XMSG_EAGAIN    (-2) /**< not now: the window is full, a slot is busy, the peer is busy */
#define XMSG_ESTATE    (-3) /**< the call is not allowed in the current state (link down, ...) */
#define XMSG_ENOSPC    (-4) /**< a fixed-size table or buffer is full */
#define XMSG_EPROTO    (-5) /**< the bytes do not form a valid frame or message */
#define XMSG_ETIMEDOUT (-6) /**< an answer did not come in time */
#define XMSG_ECANCELED (-7) /**< the caller stopped it (fa_client_cancel) */

#ifdef __cplusplus
}
#endif

#endif /* XMSG_ERRORS_H */
