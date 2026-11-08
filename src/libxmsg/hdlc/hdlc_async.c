/*
 * hdlc_async.c - byte-oriented HDLC framing: decoder state machine and resumable encoder.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * The behaviour mirrors RetroIO.Hdlc HdlcDecoder.cs / HdlcEncoder.cs, which five earlier C#
 * framers were merged into on 2026-09-06 after they disagreed with each other. The rules that
 * mattered there are the ones spelled out in hdlc_async.h: flag runs are harmless, 7D 7E is an
 * abort and not an FCS error, a partial frame at the end of a read is kept rather than reported,
 * and the sender escapes only what the escape set says.
 */
#include "libxmsg/hdlc/hdlc_async.h"

#include "libxmsg/hdlc/fcs16.h"

/* ------------------------------------------------------------------------------------------ */
/* Decoder                                                                                    */
/* ------------------------------------------------------------------------------------------ */

int hdlc_decoder_init(HdlcDecoder *d, uint8_t *buf, size_t cap, bool check_fcs)
{
    if (d == NULL || buf == NULL || cap < HDLC_MIN_FRAME_WITH_FCS)
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

/* Forget the run being collected; the next byte starts a new one (still inside the flags). */
static void decoder_start_new_run(HdlcDecoder *d)
{
    d->len = 0;
    d->escaped = false;
    d->overflowed = false;
}

void hdlc_decoder_reset(HdlcDecoder *d)
{
    decoder_start_new_run(d);
    d->seen_flag = false;
    d->frame_len = 0;
}

static void decoder_append(HdlcDecoder *d, uint8_t b)
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

/* A closing flag arrived with bytes collected: decide what the run was. */
static HdlcDecodeStatus decoder_complete_run(HdlcDecoder *d)
{
    HdlcDecodeStatus status;

    if (d->overflowed)
    {
        d->stats.too_long++;
        status = HDLC_DECODE_TOO_LONG;
    }
    else if (d->escaped)
    {
        /* The run ended right after a 0x7D. Only 7D 7E can do that, and the caller handles that
         * case before calling here; keep the branch so a future change cannot deliver a frame
         * with a dangling escape. */
        d->stats.aborts++;
        status = HDLC_DECODE_ABORTED;
    }
    else if (!d->check_fcs)
    {
        d->frame_len = d->len;
        d->stats.frames++;
        status = HDLC_DECODE_FRAME;
    }
    else if (d->len < HDLC_MIN_FRAME_WITH_FCS)
    {
        d->stats.runts++;
        status = HDLC_DECODE_RUNT;
    }
    else if (!fcs16_is_good(d->buf, d->len))
    {
        d->stats.fcs_errors++;
        status = HDLC_DECODE_FCS_ERROR;
    }
    else
    {
        d->frame_len = d->len - FCS16_SIZE;
        d->stats.frames++;
        status = HDLC_DECODE_FRAME;
    }

    decoder_start_new_run(d);
    return status;
}

HdlcDecodeStatus hdlc_decoder_feed(HdlcDecoder *d, const uint8_t *in, size_t len, size_t *consumed)
{
    size_t i;

    d->frame_len = 0;

    for (i = 0; i < len; i++)
    {
        uint8_t b = in[i];

        if (b == HDLC_FLAG)
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
                decoder_start_new_run(d);
                *consumed = i + 1;
                return HDLC_DECODE_ABORTED;
            }
            if (d->len == 0 && !d->overflowed)
            {
                /* A flag that closes nothing: inter-frame fill. */
                d->stats.idle_flags++;
                continue;
            }
            *consumed = i + 1;
            return decoder_complete_run(d);
        }

        if (!d->seen_flag)
        {
            /* Still hunting: inter-frame junk is discarded, not collected. */
            continue;
        }

        if (d->escaped)
        {
            decoder_append(d, (uint8_t)(b ^ HDLC_ESCAPE_MASK));
            d->escaped = false;
            continue;
        }

        if (b == HDLC_ESCAPE)
        {
            d->escaped = true;
            continue;
        }

        decoder_append(d, b);
    }

    *consumed = len;
    return HDLC_DECODE_NEED_MORE;
}

const uint8_t *hdlc_decoder_frame(const HdlcDecoder *d, size_t *len)
{
    if (d->frame_len == 0)
    {
        *len = 0;
        return NULL;
    }
    *len = d->frame_len;
    return d->buf;
}

/* ------------------------------------------------------------------------------------------ */
/* Encoder                                                                                    */
/* ------------------------------------------------------------------------------------------ */

enum
{
    STAGE_IDLE = 0,
    STAGE_OPEN_FLAG = 1,
    STAGE_BODY = 2,
    STAGE_FCS = 3,
    STAGE_CLOSE_FLAG = 4
};

void hdlc_encoder_init(HdlcEncoder *e, uint32_t accm, bool add_fcs)
{
    e->accm = accm;
    e->add_fcs = add_fcs;
    e->frame = NULL;
    e->len = 0;
    e->pos = 0;
    e->fcs = (uint16_t)FCS16_INIT;
    e->fcs_bytes[0] = 0;
    e->fcs_bytes[1] = 0;
    e->stage = STAGE_IDLE;
    e->half_escape = false;
    e->owed = 0;
}

bool hdlc_encoder_done(const HdlcEncoder *e)
{
    return e->stage == STAGE_IDLE;
}

int hdlc_encoder_begin(HdlcEncoder *e, const uint8_t *frame, size_t len)
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
        fcs16_append(e->fcs_bytes, fcs16_compute(frame, len));
    }
    return 0;
}

/* Does this byte need the 0x7D escape under the encoder's escape set? */
static bool encoder_must_escape(const HdlcEncoder *e, uint8_t b)
{
    if (b == HDLC_FLAG || b == HDLC_ESCAPE)
    {
        return true;
    }
    if (b < 0x20u)
    {
        return (e->accm & (1u << b)) != 0;
    }
    return false;
}

/*
 * Emit one frame byte, escaped if needed, into out[*n..cap). Returns false when there was no room
 * for the whole thing; a half-emitted escape is remembered in the encoder so the next call
 * finishes it first.
 */
static bool encoder_emit(HdlcEncoder *e, uint8_t b, uint8_t *out, size_t cap, size_t *n)
{
    if (*n >= cap)
    {
        return false;
    }
    if (!encoder_must_escape(e, b))
    {
        out[(*n)++] = b;
        return true;
    }
    out[(*n)++] = (uint8_t)HDLC_ESCAPE;
    if (*n >= cap)
    {
        e->half_escape = true;
        e->owed = (uint8_t)(b ^ HDLC_ESCAPE_MASK);
        return true; /* the byte is accounted for; its second half is owed */
    }
    out[(*n)++] = (uint8_t)(b ^ HDLC_ESCAPE_MASK);
    return true;
}

size_t hdlc_encoder_produce(HdlcEncoder *e, uint8_t *out, size_t cap)
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
            out[n++] = (uint8_t)HDLC_FLAG;
            e->stage = STAGE_BODY;
            break;

        case STAGE_BODY:
            if (e->pos < e->len)
            {
                if (!encoder_emit(e, e->frame[e->pos], out, cap, &n))
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
            if (e->pos < FCS16_SIZE)
            {
                if (!encoder_emit(e, e->fcs_bytes[e->pos], out, cap, &n))
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
            out[n++] = (uint8_t)HDLC_FLAG;
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

size_t hdlc_encode(const uint8_t *frame, size_t len, uint32_t accm, bool add_fcs, uint8_t *out,
                   size_t cap)
{
    HdlcEncoder e;
    size_t n;

    hdlc_encoder_init(&e, accm, add_fcs);
    if (hdlc_encoder_begin(&e, frame, len) != 0)
    {
        return 0;
    }
    n = hdlc_encoder_produce(&e, out, cap);
    if (!hdlc_encoder_done(&e))
    {
        /* Did not fit. Nothing partial is useful to a caller of the one-shot form. */
        return 0;
    }
    return n;
}
