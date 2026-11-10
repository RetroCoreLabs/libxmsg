/*
 * console_tad.c - a SINTRAN caller (CONNECT-TO) as a console line.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * The queue and the pump follow XMSGGW tad_in.cpp (used live against D100, 2026-11-10): output
 * and the control messages that must keep their place in it go through one queue, so a
 * password prompt never goes before the echo-off that belongs to it.
 *
 * Queue items: a byte other than 0xFF is text. 0xFF starts an item: FF FF is the text byte 0xFF;
 * FF 'E' v is the echo state (v 1 on, 0 off); FF 'S' hi lo is a SYCN value; FF 'F' is the end
 * of a burst (a frame boundary). Items are queued whole, so the pump never sees half of one.
 *
 * Locking: the queues are touched under the lock hooks. The responder's state and escape
 * callbacks only set one field each and take no lock, because tad_responder_hangup() and
 * tad_responder_abort() can call the state callback from inside the pump.
 */
#include "libxmsg/console/console_tad.h"

#include <stdio.h>
#include <string.h>

#include "libxmsg/log/xmsg_log.h"
#include "libxmsg/xmsg_errors.h"

#define MARK 0xFFu

static void lock(ConsoleTadLine *t)
{
    if (t->cfg.lock != NULL)
    {
        t->cfg.lock(t->cfg.app_ctx);
    }
}

static void unlock(ConsoleTadLine *t)
{
    if (t->cfg.unlock != NULL)
    {
        t->cfg.unlock(t->cfg.app_ctx);
    }
}

void console_tad_config_default(ConsoleTadConfig *cfg)
{
    if (cfg == NULL)
    {
        return;
    }
    memset(cfg, 0, sizeof *cfg);
    cfg->seven_bit = true;
    tad_responder_config_default(&cfg->responder);
}

/* ------------------------------------------------------------------------------------------ */
/* Responder callbacks (node side)                                                            */
/* ------------------------------------------------------------------------------------------ */

static int cb_incoming(void *ctx, const TadResponderCaller *who)
{
    ConsoleTadLine *t = (ConsoleTadLine *)ctx;
    int verdict = 0;
    if (t->cfg.incoming != NULL)
    {
        verdict = t->cfg.incoming(t->cfg.app_ctx, who);
    }
    if (verdict != 0)
    {
        return verdict;
    }
    lock(t);
    t->in_len = 0;
    t->out_len = 0;
    unlock(t);
    t->greeted = false;
    t->escape = false;
    t->close_wanted = false;
    t->had_session = false;
    t->echo_sent = 0;
    t->lost_in = 0;
    t->lost_out = 0;
    (void)snprintf(t->who, sizeof t->who, "system %u port 0x%04X", (unsigned)who->system,
                   (unsigned)who->port);
    t->state = CONSOLE_TAD_COMING;
    XMSG_LOG(XMSG_LOG_CONSOLE, XMSG_LOG_INFO, "SINTRAN caller %s for %s", t->who, who->target);
    return 0;
}

static void cb_state(void *ctx, TadResponderState st, int reason)
{
    ConsoleTadLine *t = (ConsoleTadLine *)ctx;
    XMSG_LOG(XMSG_LOG_CONSOLE, XMSG_LOG_INFO, "SINTRAN caller %s: %s (%d)", t->who,
             tad_responder_state_name(st), reason);
    if (st == TAD_RESP_RUNNING)
    {
        t->state = CONSOLE_TAD_RUNNING;
    }
    else if (st == TAD_RESP_CLOSED || st == TAD_RESP_FAILED)
    {
        t->state = CONSOLE_TAD_ENDED;
    }
}

static void cb_text_in(void *ctx, const uint8_t *bytes, size_t len)
{
    ConsoleTadLine *t = (ConsoleTadLine *)ctx;
    lock(t);
    for (size_t i = 0; i < len; i++)
    {
        if (t->in_len < sizeof t->in)
        {
            t->in[t->in_len++] = bytes[i];
        }
        else
        {
            t->lost_in++;
        }
    }
    unlock(t);
}

static void cb_escape(void *ctx)
{
    ((ConsoleTadLine *)ctx)->escape = true;
}

static void cb_status(void *ctx, uint16_t *in_use, uint16_t *total)
{
    ConsoleTadLine *t = (ConsoleTadLine *)ctx;
    t->cfg.status(t->cfg.app_ctx, in_use, total);
}

static void responder_prepare(ConsoleTadLine *t)
{
    TadResponderCallbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.ctx = t;
    cb.incoming = cb_incoming;
    cb.state = cb_state;
    cb.text_in = cb_text_in;
    cb.escape = cb_escape;
    cb.status = (t->cfg.status != NULL) ? cb_status : NULL;
    (void)tad_responder_init(&t->r, &t->cfg.responder, &cb); /* both arguments are set */
}

int console_tad_init(ConsoleTadLine *t, const ConsoleTadConfig *cfg)
{
    if (t == NULL || cfg == NULL)
    {
        return -1;
    }
    memset(t, 0, sizeof *t);
    t->cfg = *cfg;
    t->state = CONSOLE_TAD_IDLE;
    responder_prepare(t);
    return 0;
}

int console_tad_offer(ConsoleTadLine *t, XmsgNode *node, const XmsgDatagram *dg,
                      uint32_t now_ms)
{
    TadResponderState rs;
    if (t == NULL || node == NULL || dg == NULL)
    {
        return XMSG_EINVAL;
    }
    if (t->state != CONSOLE_TAD_IDLE)
    {
        return tad_responder_is_connect(dg) ? XMSG_EAGAIN : 0;
    }
    rs = tad_responder_state(&t->r);
    if (rs == TAD_RESP_CLOSED || rs == TAD_RESP_FAILED)
    {
        responder_prepare(t); /* the last call is over: a fresh responder for the next */
    }
    return tad_responder_offer(&t->r, node, dg, now_ms);
}

/* ------------------------------------------------------------------------------------------ */
/* Pump (node side)                                                                           */
/* ------------------------------------------------------------------------------------------ */

/* One item at `at` into the responder. Returns its length when taken, 0 when the responder
 * has no room now. Called with the lock held. */
static size_t pump_item(ConsoleTadLine *t, size_t at)
{
    const uint8_t *q = &t->out[at];
    int rc = XMSG_OK;
    size_t used;
    switch (q[1])
    {
    case MARK:
        rc = (tad_responder_write(&t->r, &q[1], 1) == 1) ? XMSG_OK : XMSG_EAGAIN;
        used = 2u;
        break;
    case 'E':
        if (t->echo_sent != (uint8_t)((q[2] != 0u) ? 1u : 2u))
        {
            rc = tad_responder_echo(&t->r, (q[2] != 0u) ? 0x01u : 0xFFu);
            if (rc == XMSG_OK)
            {
                t->echo_sent = (uint8_t)((q[2] != 0u) ? 1u : 2u);
            }
        }
        used = 3u;
        break;
    case 'S':
        rc = tad_responder_login_state(&t->r, (uint16_t)(((uint16_t)q[2] << 8u) | q[3]));
        used = 4u;
        break;
    default: /* 'F' */
        rc = tad_responder_send(&t->r);
        used = 2u;
        break;
    }
    return (rc == XMSG_OK) ? used : 0u;
}

/* Move the queue into the responder as far as it takes it. Called with the lock held. */
static void pump_queue(ConsoleTadLine *t)
{
    size_t at = 0;
    while (at < t->out_len)
    {
        size_t run = 0;
        int took;
        if (t->out[at] == MARK)
        {
            size_t used = pump_item(t, at);
            if (used == 0u)
            {
                break; /* every chain full: the next pass goes on */
            }
            at += used;
            continue;
        }
        while (at + run < t->out_len && t->out[at + run] != MARK)
        {
            run++;
        }
        took = tad_responder_write(&t->r, &t->out[at], run);
        if (took <= 0)
        {
            break;
        }
        at += (size_t)took;
        if ((size_t)took < run)
        {
            break;
        }
    }
    memmove(t->out, &t->out[at], t->out_len - at);
    t->out_len -= at;
}

void console_tad_pump(ConsoleTadLine *t, uint32_t now_ms)
{
    TadResponderState rs;
    bool hang_up = false;
    if (t == NULL || t->state == CONSOLE_TAD_IDLE || t->state == CONSOLE_TAD_ENDED)
    {
        return;
    }
    rs = tad_responder_state(&t->r);
    lock(t);
    if (rs == TAD_RESP_RUNNING)
    {
        if (!t->greeted)
        {
            /* As the real host starts a login (tad_responder.h): the caller's machine echoes
             * typed keys, and the login is waiting for a user. */
            t->greeted = true;
            if (tad_responder_echo(&t->r, 0x01u) == XMSG_OK)
            {
                t->echo_sent = 1u;
            }
            (void)tad_responder_login_state(&t->r, TAD_SYCN_WAIT_USER); /* a fresh chain */
        }
        pump_queue(t);
        /* The caller may type once everything we had to say is handed over (it may still be
         * in the responder's chains, ahead of the grant: text, then RFI, as the real host). */
        if (t->out_len == 0u && !t->close_wanted && !tad_responder_input_granted(&t->r))
        {
            (void)tad_responder_grant_input(&t->r); /* refused only when every chain is full */
        }
    }
    if (t->close_wanted && (t->out_len == 0u || rs != TAD_RESP_RUNNING) &&
        rs != TAD_RESP_CLOSING)
    {
        t->close_wanted = false;
        hang_up = true;
    }
    unlock(t);
    if (hang_up && tad_responder_hangup(&t->r, now_ms) != XMSG_OK)
    {
        tad_responder_abort(&t->r);
        t->state = CONSOLE_TAD_ENDED;
    }
}

/* ------------------------------------------------------------------------------------------ */
/* The line port (console side)                                                               */
/* ------------------------------------------------------------------------------------------ */

/* Queue an item whole, or nothing. Called with the lock held. */
static bool queue_item(ConsoleTadLine *t, const uint8_t *item, size_t n)
{
    if (t->out_len + n > sizeof t->out)
    {
        t->lost_out += (uint32_t)n;
        return false;
    }
    memcpy(&t->out[t->out_len], item, n);
    t->out_len += n;
    return true;
}

static int line_write(void *ctx, const uint8_t *bytes, size_t len)
{
    ConsoleTadLine *t = (ConsoleTadLine *)ctx;
    static const uint8_t ff[2] = {MARK, MARK};
    size_t i;
    lock(t);
    for (i = 0; i < len; i++)
    {
        bool ok = (bytes[i] == MARK) ? queue_item(t, ff, 2u) : queue_item(t, &bytes[i], 1u);
        if (!ok)
        {
            t->lost_out += (uint32_t)(len - i - 1u);
            break;
        }
    }
    unlock(t);
    return (int)i;
}

static void line_flush(void *ctx)
{
    ConsoleTadLine *t = (ConsoleTadLine *)ctx;
    static const uint8_t item[2] = {MARK, 'F'};
    lock(t);
    (void)queue_item(t, item, sizeof item); /* a lost boundary only joins two frames */
    unlock(t);
}

static void line_echo(void *ctx, bool on)
{
    ConsoleTadLine *t = (ConsoleTadLine *)ctx;
    const uint8_t item[3] = {MARK, 'E', (uint8_t)(on ? 1u : 0u)};
    lock(t);
    (void)queue_item(t, item, sizeof item); /* counted in lost_out when full */
    unlock(t);
}

static void line_logged_in(void *ctx, const char *user)
{
    ConsoleTadLine *t = (ConsoleTadLine *)ctx;
    const uint8_t item[4] = {MARK, 'S', (uint8_t)(TAD_SYCN_LOGGED_IN >> 8u),
                             (uint8_t)(TAD_SYCN_LOGGED_IN & 0xFFu)};
    (void)user;
    lock(t);
    (void)queue_item(t, item, sizeof item); /* counted in lost_out when full */
    unlock(t);
}

static void line_close(void *ctx, ConsoleClose why)
{
    ConsoleTadLine *t = (ConsoleTadLine *)ctx;
    (void)why;
    lock(t);
    t->session = NULL; /* the core is done with it */
    t->close_wanted = true;
    unlock(t);
}

static void line_describe(void *ctx, char *buf, size_t cap)
{
    const ConsoleTadLine *t = (const ConsoleTadLine *)ctx;
    size_t n = strlen(t->who);
    if (cap == 0u)
    {
        return;
    }
    if (n >= cap)
    {
        n = cap - 1u; /* cut to the caller's buffer */
    }
    memcpy(buf, t->who, n);
    buf[n] = '\0';
}

void console_tad_line(ConsoleTadLine *t, ConsoleLine *out)
{
    if (t == NULL || out == NULL)
    {
        return;
    }
    memset(out, 0, sizeof *out);
    out->ctx = t;
    out->caps = CONSOLE_LINE_OWN_ECHO | CONSOLE_LINE_ESCAPE_MSG |
                (t->cfg.seven_bit ? CONSOLE_LINE_7BIT : 0u);
    out->kind = "TAD";
    out->write = line_write;
    out->flush = line_flush;
    out->echo = line_echo;
    out->logged_in = line_logged_in;
    out->close = line_close;
    out->describe = line_describe;
}

void console_tad_attach(ConsoleTadLine *t, ConsoleSession *s)
{
    if (t != NULL)
    {
        t->session = s;
        if (s != NULL)
        {
            t->had_session = true;
        }
    }
}

bool console_tad_needs_session(const ConsoleTadLine *t)
{
    return t != NULL && t->state == CONSOLE_TAD_RUNNING && !t->had_session && !t->close_wanted;
}

ConsoleTadState console_tad_deliver(ConsoleTadLine *t, uint32_t now_ms)
{
    uint8_t buf[CONSOLE_TAD_IN_MAX];
    size_t n;
    bool escape;
    if (t == NULL)
    {
        return CONSOLE_TAD_IDLE;
    }
    if (t->session == NULL)
    {
        return t->state;
    }
    lock(t);
    n = t->in_len;
    memcpy(buf, t->in, n);
    t->in_len = 0;
    escape = t->escape;
    t->escape = false;
    unlock(t);
    if (escape)
    {
        console_escape(t->session);
    }
    if (n > 0u)
    {
        console_input(t->session, buf, n, now_ms); /* may close the line: session NULL then */
    }
    if (t->state == CONSOLE_TAD_ENDED && t->session != NULL)
    {
        console_line_lost(t->session);
        t->session = NULL;
    }
    return t->state;
}

bool console_tad_release(ConsoleTadLine *t)
{
    if (t == NULL)
    {
        return false;
    }
    if (t->state == CONSOLE_TAD_ENDED && t->session == NULL)
    {
        t->state = CONSOLE_TAD_IDLE;
    }
    return t->state == CONSOLE_TAD_IDLE;
}

ConsoleTadState console_tad_state(const ConsoleTadLine *t)
{
    return (t == NULL) ? CONSOLE_TAD_IDLE : t->state;
}
