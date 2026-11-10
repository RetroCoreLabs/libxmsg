/**************************************************************************
** BYTE_FRAME - HDLC frames on a byte stream (RFC 1662 style)            **
**                                                                       **
** The engine came from libxmsg hdlc_async.c (2026-11-09), which mirrors **
** RetroIO.Hdlc HdlcDecoder.cs / HdlcEncoder.cs; five earlier C# framers **
** were merged into those on 2026-09-06 after they disagreed with each   **
** other. The rules that mattered there are the ones in byte_frame.h:    **
** flag runs are harmless, 7D 7E is an abort and not an FCS error, a     **
** partial frame at the end of a read is kept rather than reported, and  **
** the sender escapes only what the escape set says.                     **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#include "libhdlc/byte_frame.h"
#include "libhdlc/hot_path.h"

#include <stddef.h>

/* ------------------------------------------------------------ decoder */

int byte_frame_rx_init(byte_frame_rx *d, uint8_t *buf, size_t cap, bool check_fcs)
{
    if (d == NULL || buf == NULL || cap < BYTE_FRAME_MIN_WITH_FCS)
    {
        return -1;
    }
    d->buf = buf;
    d->cap = cap;
    d->len = 0;
    d->frame_len = 0;
    d->check_fcs = check_fcs;
    d->seen_flag = false;
    d->escaped = false;
    d->overflowed = false;
    d->stats.frames = 0;
    d->stats.fcs_errors = 0;
    d->stats.runts = 0;
    d->stats.too_long = 0;
    d->stats.aborts = 0;
    d->stats.idle_flags = 0;
    return 0;
}

/**
 * @brief Forget the run being collected; the next byte starts a new one (still inside the flags).
 * @param[in,out] d  Decoder.
 */
static HOT_PATH_CODE void start_new_run(byte_frame_rx *d)
{
    d->len = 0;
    d->escaped = false;
    d->overflowed = false;
}

void byte_frame_rx_reset(byte_frame_rx *d)
{
    start_new_run(d);
    d->seen_flag = false;
    d->frame_len = 0;
}

/**
 * @brief Keep one un-escaped byte, or mark the run too long.
 * @param[in,out] d  Decoder.
 * @param[in]     b  The byte.
 */
static HOT_PATH_CODE void append(byte_frame_rx *d, uint8_t b)
{
    if (d->len < d->cap)
    {
        d->buf[d->len++] = b;
    }
    else
    {
        /* Keep collecting state but not bytes: the run is dropped when its flag arrives, so the
         * decoder resynchronises on the next frame boundary instead of in the middle of data. */
        d->overflowed = true;
    }
}

/**
 * @brief A closing flag arrived with bytes collected: decide what the run was.
 * @param[in,out] d  Decoder.
 * @return The run's status.
 */
static HOT_PATH_CODE byte_frame_rx_status complete_run(byte_frame_rx *d)
{
    byte_frame_rx_status status;

    if (d->overflowed)
    {
        d->stats.too_long++;
        status = BYTE_FRAME_RX_TOO_LONG;
    }
    else if (d->escaped)
    {
        /* The run ended right after a 0x7D. Only 7D 7E can do that, and the caller handles that
         * case before calling here; keep the branch so a future change cannot deliver a frame
         * with a dangling escape. */
        d->stats.aborts++;
        status = BYTE_FRAME_RX_ABORTED;
    }
    else if (!d->check_fcs)
    {
        d->frame_len = d->len;
        d->stats.frames++;
        status = BYTE_FRAME_RX_FRAME;
    }
    else if (d->len < BYTE_FRAME_MIN_WITH_FCS)
    {
        d->stats.runts++;
        status = BYTE_FRAME_RX_RUNT;
    }
    else if (!hdlc_fcs_is_good(d->buf, d->len))
    {
        d->stats.fcs_errors++;
        status = BYTE_FRAME_RX_FCS_ERROR;
    }
    else
    {
        d->frame_len = d->len - HDLC_FCS_SIZE;
        d->stats.frames++;
        status = BYTE_FRAME_RX_FRAME;
    }
    start_new_run(d);
    return status;
}

byte_frame_rx_status HOT_PATH_CODE byte_frame_rx_feed(byte_frame_rx *d, const uint8_t *in,
                                                      size_t len, size_t *consumed)
{
    size_t i;

    d->frame_len = 0;
    for (i = 0; i < len; i++)
    {
        uint8_t b = in[i];
        if (b == BYTE_FRAME_FLAG)
        {
            if (!d->seen_flag)
            {
                /* The opening flag we were hunting for. Nothing before it was a frame. */
                d->seen_flag = true;
                d->stats.idle_flags++;
                continue;
            }
            if (d->escaped)
            {
                /* 7D 7E: abort. Drop the run silently; not an FCS error. */
                d->stats.aborts++;
                start_new_run(d);
                *consumed = i + 1u;
                return BYTE_FRAME_RX_ABORTED;
            }
            if (d->len == 0 && !d->overflowed)
            {
                /* A flag that closes nothing: inter-frame fill. */
                d->stats.idle_flags++;
                continue;
            }
            *consumed = i + 1u;
            return complete_run(d);
        }
        if (!d->seen_flag)
        {
            /* Still hunting: inter-frame junk is discarded, not collected. */
            continue;
        }
        if (d->escaped)
        {
            append(d, (uint8_t)(b ^ BYTE_FRAME_ESCAPE_MASK));
            d->escaped = false;
            continue;
        }
        if (b == BYTE_FRAME_ESCAPE)
        {
            d->escaped = true;
            continue;
        }
        append(d, b);
    }
    *consumed = len;
    return BYTE_FRAME_RX_NEED_MORE;
}

const uint8_t *byte_frame_rx_frame(const byte_frame_rx *d, size_t *len)
{
    if (d->frame_len == 0)
    {
        *len = 0;
        return NULL;
    }
    *len = d->frame_len;
    return d->buf;
}

/* ------------------------------------------------------------ encoder */

enum
{
    STAGE_IDLE = 0,
    STAGE_OPEN_FLAG = 1,
    STAGE_BODY = 2,
    STAGE_FCS = 3,
    STAGE_CLOSE_FLAG = 4
};

void byte_frame_tx_init(byte_frame_tx *e, uint32_t accm, bool add_fcs)
{
    e->accm = accm;
    e->add_fcs = add_fcs;
    e->frame = NULL;
    e->len = 0;
    e->pos = 0;
    e->fcs_bytes[0] = 0;
    e->fcs_bytes[1] = 0;
    e->stage = STAGE_IDLE;
    e->half_escape = false;
    e->owed = 0;
}

bool byte_frame_tx_done(const byte_frame_tx *e)
{
    return e->stage == STAGE_IDLE;
}

int HOT_PATH_CODE byte_frame_tx_begin(byte_frame_tx *e, const uint8_t *frame, size_t len)
{
    if (e->stage != STAGE_IDLE || frame == NULL || len == 0)
    {
        return -1;
    }
    e->frame = frame;
    e->len = len;
    e->pos = 0;
    e->stage = STAGE_OPEN_FLAG;
    e->half_escape = false;
    if (e->add_fcs)
    {
        /* The FCS covers the frame as given (before escaping), so it can be computed up front;
         * escaping is applied to the FCS bytes too when they land on 7E, 7D or a masked byte. */
        hdlc_fcs_append(e->fcs_bytes, hdlc_fcs_compute(frame, len));
    }
    return 0;
}

/**
 * @brief Does this byte need the 0x7D escape under the encoder's escape set?
 * @param[in] e  Encoder.
 * @param[in] b  Byte.
 * @return true when it must be escaped.
 */
static HOT_PATH_CODE bool must_escape(const byte_frame_tx *e, uint8_t b)
{
    if (b == BYTE_FRAME_FLAG || b == BYTE_FRAME_ESCAPE)
    {
        return true;
    }
    if (b < 0x20u)
    {
        return (e->accm & (1u << b)) != 0u;
    }
    return false;
}

/**
 * @brief Emit one frame byte, escaped if needed, into out[*n..cap).
 *
 * A half-emitted escape is remembered in the encoder so the next produce call finishes it first.
 *
 * @param[in,out] e    Encoder.
 * @param[in]     b    Byte.
 * @param[out]    out  Output buffer.
 * @param[in]     cap  Its size.
 * @param[in,out] n    Bytes already in out.
 * @return false when there was no room even for the first output byte.
 */
static HOT_PATH_CODE bool emit(byte_frame_tx *e, uint8_t b, uint8_t *out, size_t cap, size_t *n)
{
    if (*n >= cap)
    {
        return false;
    }
    if (!must_escape(e, b))
    {
        out[(*n)++] = b;
        return true;
    }
    out[(*n)++] = (uint8_t)BYTE_FRAME_ESCAPE;
    if (*n >= cap)
    {
        e->half_escape = true;
        e->owed = (uint8_t)(b ^ BYTE_FRAME_ESCAPE_MASK);
        return true; /* the byte is accounted for; its second half is owed */
    }
    out[(*n)++] = (uint8_t)(b ^ BYTE_FRAME_ESCAPE_MASK);
    return true;
}

size_t HOT_PATH_CODE byte_frame_tx_produce(byte_frame_tx *e, uint8_t *out, size_t cap)
{
    size_t n = 0;

    if (e->stage == STAGE_IDLE || out == NULL || cap == 0)
    {
        return 0;
    }
    if (e->half_escape)
    {
        out[n++] = e->owed;
        e->half_escape = false;
    }
    while (n < cap && e->stage != STAGE_IDLE)
    {
        switch (e->stage)
        {
        case STAGE_OPEN_FLAG:
            out[n++] = (uint8_t)BYTE_FRAME_FLAG;
            e->stage = STAGE_BODY;
            break;
        case STAGE_BODY:
            if (e->pos < e->len)
            {
                if (!emit(e, e->frame[e->pos], out, cap, &n))
                {
                    return n;
                }
                e->pos++;
            }
            else
            {
                e->pos = 0;
                e->stage = e->add_fcs ? (uint8_t)STAGE_FCS : (uint8_t)STAGE_CLOSE_FLAG;
            }
            break;
        case STAGE_FCS:
            if (e->pos < HDLC_FCS_SIZE)
            {
                if (!emit(e, e->fcs_bytes[e->pos], out, cap, &n))
                {
                    return n;
                }
                e->pos++;
            }
            else
            {
                e->stage = STAGE_CLOSE_FLAG;
            }
            break;
        case STAGE_CLOSE_FLAG:
            out[n++] = (uint8_t)BYTE_FRAME_FLAG;
            e->stage = STAGE_IDLE;
            e->frame = NULL;
            break;
        default:
            e->stage = STAGE_IDLE;
            break;
        }
    }
    return n;
}

size_t HOT_PATH_CODE byte_frame_encode_accm(const uint8_t *frame, size_t len, uint32_t accm,
                                            bool add_fcs, uint8_t *out, size_t cap)
{
    byte_frame_tx e;
    size_t n;

    byte_frame_tx_init(&e, accm, add_fcs);
    if (byte_frame_tx_begin(&e, frame, len) != 0)
    {
        return 0;
    }
    n = byte_frame_tx_produce(&e, out, cap);
    if (!byte_frame_tx_done(&e))
    {
        /* Did not fit. Nothing partial is useful to a caller of the one-shot form. */
        return 0;
    }
    return n;
}

/* --------------------------------------------------------- short form */

int HOT_PATH_CODE byte_frame_encode(const uint8_t *frame, int len, uint8_t *out)
{
    if (len <= 0)
    {
        /* The old encoder wrote flag, nothing, flag; kept so no caller sees a change. */
        out[0] = BYTE_FRAME_FLAG;
        out[1] = BYTE_FRAME_FLAG;
        return 2;
    }
    return (int)byte_frame_encode_accm(frame, (size_t)len, BYTE_FRAME_ACCM_ALL_CONTROL, false,
                                       out, 2u + (2u * (size_t)len));
}

void byte_frame_decoder_reset(byte_frame_decoder *dec)
{
    /* Point the engine at the built-in buffer without clearing its counters, so a reconnect
     * keeps them, the same as overlong. */
    dec->rx.buf = dec->buffer;
    dec->rx.cap = BYTE_FRAME_MAX;
    dec->rx.check_fcs = false;
    byte_frame_rx_reset(&dec->rx);
    dec->length = 0;
}

bool HOT_PATH_CODE byte_frame_decoder_feed(byte_frame_decoder *dec, uint8_t data)
{
    size_t used;
    byte_frame_rx_status st;

    if (dec->rx.buf == NULL)
    {
        byte_frame_decoder_reset(dec);   /* zeroed storage, never reset */
    }
    st = byte_frame_rx_feed(&dec->rx, &data, 1u, &used);
    if (st == BYTE_FRAME_RX_FRAME)
    {
        dec->length = (int)dec->rx.frame_len;
        return true;
    }
    if (st == BYTE_FRAME_RX_TOO_LONG)
    {
        /* Shown by STAT (2026-11-09): until then a too-long frame from the host or a TCP peer
         * vanished without a trace. */
        dec->overlong = dec->overlong + 1u;
    }
    dec->length = (int)dec->rx.len;
    return false;
}

bool byte_frame_decoder_is_idle(const byte_frame_decoder *dec)
{
    return !dec->rx.seen_flag || dec->rx.len == 0;
}
