/*
 * fa_client.c - a COSMOS file-access client: read, write or list files on a SINTRAN machine.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libcosmos/fa/fa_client.h"

#include <string.h>

#include "libcosmos/sintran/sintran_password.h"
#include "libxmsg/log/xmsg_log.h"
#include "libxmsg/sintran/sintran_hdr.h"
#include "libxmsg/xmsg_errors.h"

/* XENSE ("your number is ahead") resends of one frame before giving up, as the TAD asker. */
#define FA_CLIENT_XENSE_TRIES 3u

static const XmsgSessionOps s_fa_client_ops;

/* ------------------------------------------------------------------------------------------ */
/* State and queue                                                                            */
/* ------------------------------------------------------------------------------------------ */

static void update_wants_tx(FaClient *c)
{
    c->base.wants_tx = (c->inflight.kind == FA_SEND_NONE) && (c->q_count > 0u);
}

static void finish_with(FaClient *c, FaClientState st, int result, uint32_t now_ms)
{
    (void)now_ms;
    if (c->state == FA_CLIENT_DONE || c->state == FA_CLIENT_FAILED)
    {
        return;
    }
    if (c->cancelled && st == FA_CLIENT_DONE)
    {
        result = XMSG_ECANCELED; /* the teardown ended normally after fa_client_cancel */
    }
    XMSG_LOG(XMSG_LOG_COSMOS, (result == 0) ? XMSG_LOG_INFO : XMSG_LOG_WARN,
             "fa client %s -> %s (result %d, SINTRAN error %u)", fa_client_state_name(c->state),
             fa_client_state_name(st), result, (unsigned)c->sintran_error);
    c->state = st;
    c->result = result;
    c->q_count = 0;
    c->inflight.kind = FA_SEND_NONE;
    c->base.wants_tx = false;
    if (c->base.attached && c->base.node != NULL)
    {
        (void)xmsg_node_detach(c->base.node, &c->base); /* safe inside node callbacks */
    }
    if (c->cb.done != NULL)
    {
        c->cb.done(c->cb.ctx, result, c->sintran_error);
    }
}

static void fail(FaClient *c, int result, uint32_t now_ms)
{
    finish_with(c, FA_CLIENT_FAILED, result, now_ms);
}

static void enqueue(FaClient *c, FaClientSend kind, uint8_t counter, uint16_t seq,
                    uint32_t now_ms)
{
    FaClientItem *it;

    if (c->q_count >= FA_CLIENT_QUEUE)
    {
        XMSG_LOG(XMSG_LOG_COSMOS, XMSG_LOG_ERROR, "fa client send queue full (%u)",
                 (unsigned)FA_CLIENT_QUEUE);
        fail(c, XMSG_ENOSPC, now_ms);
        return;
    }
    it = &c->queue[(c->q_head + c->q_count) % FA_CLIENT_QUEUE];
    it->kind = (uint8_t)kind;
    it->counter = counter;
    it->seq = seq;
    c->q_count++;
    update_wants_tx(c);
}

/* Queue a short ack for one message from the server. */
static void enqueue_ack(FaClient *c, uint32_t now_ms)
{
    c->ack_counter = (uint8_t)(c->ack_counter + 1u);
    enqueue(c, FA_SEND_ACK, c->ack_counter, 0, now_ms);
}

/* Queue the request of the current step, giving it its counter and sequence now so a resend
 * after XENSE goes out with the same numbers. */
static void enqueue_request(FaClient *c, uint32_t now_ms)
{
    uint16_t seq;

    c->seq = (uint16_t)(c->seq + 1u);
    seq = c->seq;
    /* MEASURED: block requests alternate the 0x8000 bit, clear on the first block (read blocks
     * 0005 8006 0007 8008 .. in capture-read.txt; RetroIO FaReadLadder.AlternatingBlockSequenceBit,
     * which leaves it out; the real client sends it, so this does). */
    if ((c->step == FA_STEP_READ || c->step == FA_STEP_WRITE) && (c->block & 1u) != 0u)
    {
        /* MEASURED the same for writes: 0004 8005 0006 .. in capture-write.txt. */
        seq = (uint16_t)(seq | FA_SEQ_ALTERNATE_BIT);
    }
    c->reply_in = false;
    c->data_in = 0;
    enqueue(c, FA_SEND_REQUEST, fa_counter_take(&c->counter, false), seq, now_ms);
    if (c->step == FA_STEP_WRITE)
    {
        /* MEASURED (capture-write.txt): the block's two data messages follow the request at
         * once, before the server has answered it; the counter runs on, bit 7 clear on the
         * first half only (83 04 85, 86 07 88, ..). */
        enqueue(c, FA_SEND_DATA, fa_counter_take(&c->counter, true), 0, now_ms);
        enqueue(c, FA_SEND_DATA, fa_counter_take(&c->counter, false), 1, now_ms);
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Building frames                                                                            */
/* ------------------------------------------------------------------------------------------ */

static size_t build_request(FaClient *c, const FaClientItem *it, uint8_t *body, size_t cap)
{
    FaEnvelope env;

    env.type = FA_TYPE_REQUEST;
    env.conversation = c->server_conversation;
    env.counter = it->counter;
    env.token = c->first_request_sent ? FA_TOKEN_CLIENT : FA_TOKEN_CLIENT_FIRST;
    switch (c->step)
    {
    case FA_STEP_RESERVE:
        return fa_build_reserve(body, cap, &env, it->seq, c->cfg.asker, c->local_user,
                                c->remote_user, c->password);
    case FA_STEP_OPEN:
        if (c->writing)
        {
            return fa_build_open_write(body, cap, &env, it->seq, c->spec, c->create);
        }
        return fa_build_open_read(body, cap, &env, it->seq, c->spec);
    case FA_STEP_SET_BLOCK_SIZE:
        return fa_build_set_block_size(body, cap, &env, it->seq, (uint16_t)FA_BLOCK_SIZE);
    case FA_STEP_FILE_INFO:
        return fa_build_file_info(body, cap, &env, it->seq);
    case FA_STEP_READ:
        return fa_build_read_block(body, cap, &env, it->seq, c->block);
    case FA_STEP_CLOSE:
        return fa_build_bare(body, cap, &env, FA_OP_CLOSE, it->seq);
    case FA_STEP_RELEASE:
        return fa_build_bare(body, cap, &env, FA_OP_RELEASE, it->seq);
    case FA_STEP_WALK:
        return fa_build_walk_next(body, cap, &env, it->seq, c->walk_block, c->user_index,
                                  c->cursor);
    case FA_STEP_DIRECTORY:
        /* MEASURED: index 0 in both captured listings (users SYSTEM and SECRET). */
        return fa_build_special_index(body, cap, &env, it->seq, FA_SPECIAL_DIRECTORY, 0);
    case FA_STEP_USER:
        return fa_build_special_index(body, cap, &env, it->seq, FA_SPECIAL_USER, c->user_index);
    case FA_STEP_WRITE:
        return fa_build_write_block(body, cap, &env, it->seq, c->block);
    case FA_STEP_END_OF_FILE:
        return fa_build_set_end_of_file(body, cap, &env, it->seq, c->length);
    default:
        return 0;
    }
}

/* One half of the block being written: the envelope and 1024 bytes from the source, the end
 * of the file padded with zeros (MEASURED: the last message of capture-write.txt carries 497
 * file bytes and then 527 zeros). Returns 0 when the source fails. */
static size_t build_data(FaClient *c, const FaClientItem *it, uint8_t *body, size_t cap)
{
    FaEnvelope env;
    uint32_t offset = c->block * FA_BLOCK_SIZE + ((it->seq != 0u) ? FA_DATA_BYTES : 0u);
    size_t n = 0;

    if (cap < FA_DATA_MESSAGE_SIZE || c->cb.source == NULL)
    {
        return 0;
    }
    env.type = FA_TYPE_REQUEST;
    env.conversation = c->server_conversation;
    env.counter = it->counter;
    env.token = (it->seq != 0u) ? FA_TOKEN_CLIENT_DATA_SECOND : FA_TOKEN_CLIENT;
    fa_envelope_write(body, &env);
    memset(body + FA_ENVELOPE_SIZE, 0, FA_DATA_BYTES);
    if (offset < c->length)
    {
        n = c->length - offset;
        if (n > FA_DATA_BYTES)
        {
            n = FA_DATA_BYTES;
        }
        if (c->cb.source(c->cb.ctx, offset, body + FA_ENVELOPE_SIZE, n) != n)
        {
            XMSG_LOG(XMSG_LOG_COSMOS, XMSG_LOG_ERROR,
                     "fa client: the source gave less than %u bytes at %u", (unsigned)n,
                     (unsigned)offset);
            return 0;
        }
    }
    return FA_DATA_MESSAGE_SIZE;
}

static int send_item(FaClient *c, const FaClientItem *it, uint32_t now_ms)
{
    XmsgNode *n = c->base.node;
    XmsgSubhdr sub;
    uint8_t *body;
    size_t cap;
    size_t len = 0;
    uint16_t flags1;
    int rc;

    rc = xmsg_node_send_begin(n, &body, &cap);
    if (rc != XMSG_OK)
    {
        return rc;
    }
    memset(&sub, 0, sizeof sub);
    sub.dst_system = c->base.peer_system;
    sub.src_port = c->base.local_port;
    /* MEASURED (capture-read.txt): every frame after the letter is 82/84 to the session port. */
    sub.frame_flags = XMSG_FF_CONTROL_BARE;
    sub.role = FA_ROLE_DATA;
    sub.dst_port = c->base.peer_port;
    switch ((FaClientSend)it->kind)
    {
    case FA_SEND_LETTER:
        len = fa_build_connect_letter(body, cap, c->system_name, c->cfg.word);
        sub.frame_flags = XMSG_FF_SETUP;
        sub.role = FA_ROLE_LETTER;
        sub.dst_port = 0;
        break;
    case FA_SEND_REQUEST:
        len = build_request(c, it, body, cap);
        break;
    case FA_SEND_ACK:
        len = fa_build_short_ack(body, cap, c->server_conversation, it->counter);
        break;
    case FA_SEND_FINISH:
        len = fa_build_session_finished(body, cap, c->server_conversation, c->cfg.word);
        break;
    case FA_SEND_DATA:
        len = build_data(c, it, body, cap);
        if (len == 0u)
        {
            xmsg_node_send_cancel(n);
            return XMSG_EIO;
        }
        break;
    default:
        break;
    }
    if (len == 0u)
    {
        xmsg_node_send_cancel(n);
        return XMSG_EINVAL;
    }
    sub.xmcsm = (uint16_t)len; /* MEASURED: the body length on every FA frame */
    rc = xmsg_node_send_commit(n, c->base.peer_node, &sub, len, now_ms, &flags1);
    if (rc != XMSG_OK)
    {
        xmsg_node_send_cancel(n);
        return rc;
    }
    if (it->kind == FA_SEND_REQUEST)
    {
        c->first_request_sent = true;
    }
    c->inflight = *it;
    c->inflight_flags1 = flags1;
    c->inflight_since_ms = now_ms;
    c->stats.frames_out++;
    return XMSG_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* The ladder                                                                                 */
/* ------------------------------------------------------------------------------------------ */

/* Whether the file is open on the server while this step runs (the open has succeeded). */
static bool step_has_open_file(FaClientStep step)
{
    switch (step)
    {
    case FA_STEP_SET_BLOCK_SIZE:
        /* fall through */
    case FA_STEP_FILE_INFO:
        /* fall through */
    case FA_STEP_READ:
        /* fall through */
    case FA_STEP_WRITE:
        /* fall through */
    case FA_STEP_END_OF_FILE:
        return true;
    default:
        return false;
    }
}

/* After fa_client_cancel the step that just completed leads to the teardown: close an open
 * file, then release, then session finished. */
static FaClientStep cancel_step(const FaClient *c)
{
    if (c->step == FA_STEP_OPEN || step_has_open_file(c->step))
    {
        return FA_STEP_CLOSE; /* the open succeeded: the file is open */
    }
    return FA_STEP_RELEASE;
}

/* The current step is complete: go on to the next one. */
static void next_step(FaClient *c, uint32_t now_ms)
{
    if (c->cancelled && c->step != FA_STEP_RELEASE)
    {
        c->step = cancel_step(c);
        c->wait_since_ms = now_ms;
        enqueue_request(c, now_ms);
        return;
    }
    switch (c->step)
    {
    case FA_STEP_RESERVE:
        c->step = c->listing ? FA_STEP_WALK : FA_STEP_OPEN;
        break;
    case FA_STEP_WALK:
        /* As the real client: after the first file, ask once for the directory and user names. */
        c->step = c->names_asked ? FA_STEP_WALK : FA_STEP_DIRECTORY;
        c->names_asked = true;
        break;
    case FA_STEP_DIRECTORY:
        c->step = FA_STEP_USER;
        break;
    case FA_STEP_USER:
        c->step = FA_STEP_WALK;
        break;
    case FA_STEP_OPEN:
        c->step = FA_STEP_SET_BLOCK_SIZE;
        break;
    case FA_STEP_SET_BLOCK_SIZE:
        c->step = c->writing ? FA_STEP_WRITE : FA_STEP_FILE_INFO;
        c->block = 0;
        break;
    case FA_STEP_WRITE:
        c->block++;
        if (c->block >= c->blocks)
        {
            c->step = FA_STEP_END_OF_FILE;
        }
        break;
    case FA_STEP_END_OF_FILE:
        c->step = FA_STEP_CLOSE;
        break;
    case FA_STEP_FILE_INFO:
        c->step = FA_STEP_READ;
        c->block = 0;
        break;
    case FA_STEP_READ:
        c->block++;
        if (c->block >= c->blocks)
        {
            c->step = FA_STEP_CLOSE;
        }
        break;
    case FA_STEP_CLOSE:
        c->step = FA_STEP_RELEASE;
        break;
    case FA_STEP_RELEASE:
    default:
        /* The last word: the server answers it with 07C0 and frees its seat. */
        enqueue(c, FA_SEND_FINISH, 0, 0, now_ms);
        c->state = FA_CLIENT_FINISHING;
        c->wait_since_ms = now_ms;
        return;
    }
    c->wait_since_ms = now_ms;
    enqueue_request(c, now_ms);
}

/* Jump to a step (after a refusal) and send its request. */
static void go_to_step(FaClient *c, FaClientStep step, uint32_t now_ms)
{
    c->step = step;
    c->wait_since_ms = now_ms;
    enqueue_request(c, now_ms);
}

static bool step_is_complete(const FaClient *c)
{
    return c->reply_in && (c->step != FA_STEP_READ || c->data_in >= 2u);
}

/* One file of a walk: report it and move the cursor past it. */
static bool take_walk_entry(FaClient *c, const FaReply *r)
{
    const uint8_t *raw;
    uint16_t user_index;
    uint16_t object_index;
    FaFileEntry e;

    if (!fa_reply_walk_entry(r, &user_index, &object_index, &raw))
    {
        return false;
    }
    c->user_index = user_index;
    c->cursor = (uint16_t)(object_index + 1u);
    c->entries++;
    if (c->cancelled)
    {
        return true; /* answered after fa_client_cancel: not reported */
    }
    fa_entry_parse(raw, &e);
    e.object_index = object_index;
    XMSG_LOG(XMSG_LOG_COSMOS, XMSG_LOG_DEBUG, "fa client: entry %u %s:%s, %u bytes",
             (unsigned)object_index, e.name, e.type, (unsigned)e.bytes);
    if (c->cb.entry != NULL)
    {
        c->cb.entry(c->cb.ctx, &e);
    }
    return true;
}

static void on_reply(FaClient *c, const uint8_t *body, size_t len, uint32_t now_ms)
{
    static const uint16_t step_ops[] = {FA_OP_RESERVE, FA_OP_OPEN,    FA_OP_SET_BLOCK_SIZE,
                                        FA_OP_SPECIAL, FA_OP_READ,    FA_OP_CLOSE,
                                        FA_OP_RELEASE, FA_OP_SPECIAL, FA_OP_SPECIAL,
                                        FA_OP_SPECIAL, FA_OP_WRITE,   FA_OP_SPECIAL};
    FaReply r;

    memset(&r, 0, sizeof r);
    if (!fa_reply_parse(body, len, &r) || c->reply_in || r.op != step_ops[c->step])
    {
        c->stats.ignored++;
        XMSG_LOG(XMSG_LOG_COSMOS, XMSG_LOG_WARN,
                 "fa client: unexpected reply (%u bytes, op 0x%04X) in step %d, ignored",
                 (unsigned)len, (unsigned)r.op, (int)c->step);
        return;
    }
    enqueue_ack(c, now_ms);
    c->reply_in = true;
    if (r.refused && c->step == FA_STEP_WALK && r.error == FA_ERR_END_OF_DIRECTORY)
    {
        /* The normal end of a walk (capture-list-files.txt 02:20:17.043). */
        XMSG_LOG(XMSG_LOG_COSMOS, XMSG_LOG_INFO, "fa client: end of directory after %u files",
                 (unsigned)c->entries);
        go_to_step(c, FA_STEP_RELEASE, now_ms);
        return;
    }
    if (r.refused && c->sintran_error == 0u)
    {
        XMSG_LOG(XMSG_LOG_COSMOS, XMSG_LOG_WARN, "fa client: op 0x%04X refused: error %u %s",
                 (unsigned)r.op, (unsigned)r.error, fa_error_name(r.error));
        c->sintran_error = r.error;
        /* Give the seat back: close what is open, then release, then session finished.
         * MEASURED for a refused open: release follows at once (capture-open-error.txt).
         * A refused reserve also goes to release: no capture shows it; release is what frees
         * the seat, so it is sent (unverified). */
        if (c->step == FA_STEP_RESERVE || c->step == FA_STEP_OPEN || c->listing)
        {
            go_to_step(c, FA_STEP_RELEASE, now_ms);
        }
        else if (step_has_open_file(c->step))
        {
            go_to_step(c, FA_STEP_CLOSE, now_ms);
        }
        else
        {
            next_step(c, now_ms);
        }
        return;
    }
    if (c->step == FA_STEP_OPEN && c->writing)
    {
        if (!fa_reply_file_number(&r, &c->file_number))
        {
            XMSG_LOG(XMSG_LOG_COSMOS, XMSG_LOG_ERROR, "fa client: open reply without a file");
            fail(c, XMSG_EPROTO, now_ms);
            return;
        }
        XMSG_LOG(XMSG_LOG_COSMOS, XMSG_LOG_INFO, "fa client: writing %s, %u bytes, %u blocks",
                 c->spec, (unsigned)c->length, (unsigned)c->blocks);
    }
    else if (c->step == FA_STEP_OPEN)
    {
        if (!fa_reply_open_info(&r, &c->file_number, &c->length))
        {
            XMSG_LOG(XMSG_LOG_COSMOS, XMSG_LOG_ERROR, "fa client: open reply without a length");
            fail(c, XMSG_EPROTO, now_ms);
            return;
        }
        c->blocks = (c->length + FA_BLOCK_SIZE - 1u) / FA_BLOCK_SIZE;
        if (c->blocks == 0u)
        {
            /* An empty file is still read as one block (RetroIO BlockCountForLength). */
            c->blocks = 1;
        }
        XMSG_LOG(XMSG_LOG_COSMOS, XMSG_LOG_INFO, "fa client: %s is %u bytes, %u blocks", c->spec,
                 (unsigned)c->length, (unsigned)c->blocks);
        if (c->cb.opened != NULL && !c->cancelled)
        {
            c->cb.opened(c->cb.ctx, c->length);
        }
    }
    else if (c->step == FA_STEP_WALK)
    {
        if (!take_walk_entry(c, &r))
        {
            XMSG_LOG(XMSG_LOG_COSMOS, XMSG_LOG_ERROR, "fa client: walk reply without an entry");
            fail(c, XMSG_EPROTO, now_ms);
            return;
        }
    }
    else if (c->step == FA_STEP_DIRECTORY)
    {
        (void)fa_reply_entry_name(&r, 10u, c->directory_name, sizeof c->directory_name);
    }
    else if (c->step == FA_STEP_USER)
    {
        (void)fa_reply_entry_name(&r, 0u, c->user_name, sizeof c->user_name);
        if (c->cb.directory != NULL && !c->cancelled)
        {
            c->cb.directory(c->cb.ctx, c->directory_name, c->user_name);
        }
    }
    else if (c->step == FA_STEP_FILE_INFO)
    {
        const uint8_t *entry = fa_reply_object_entry(&r);
        if (entry != NULL)
        {
            memcpy(c->object_entry, entry, sizeof c->object_entry);
            c->has_object_entry = true;
        }
    }
    if (step_is_complete(c))
    {
        next_step(c, now_ms);
    }
}

static void on_data(FaClient *c, const uint8_t *body, size_t len, uint32_t now_ms)
{
    uint32_t offset;
    bool second = (body[4] & 0x80u) != 0u; /* bit 7 clear on the first half of a pair */

    (void)len;
    if (c->step != FA_STEP_READ || !c->reply_in || c->data_in >= 2u)
    {
        c->stats.ignored++;
        XMSG_LOG(XMSG_LOG_COSMOS, XMSG_LOG_WARN, "fa client: data message outside a block read");
        return;
    }
    enqueue_ack(c, now_ms);
    c->data_in++;
    offset = c->block * FA_BLOCK_SIZE + (second ? FA_DATA_BYTES : 0u);
    if (offset < c->length && c->cb.data != NULL && !c->cancelled)
    {
        uint32_t n = c->length - offset;
        if (n > FA_DATA_BYTES)
        {
            n = FA_DATA_BYTES;
        }
        c->cb.data(c->cb.ctx, offset, body + FA_ENVELOPE_SIZE, n);
    }
    if (step_is_complete(c))
    {
        next_step(c, now_ms);
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Session operations                                                                         */
/* ------------------------------------------------------------------------------------------ */

static void client_on_datagram(XmsgSession *s, const XmsgDatagram *dg, uint32_t now_ms)
{
    FaClient *c = (FaClient *)s;
    FaEnvelope e;

    if (dg->repeat)
    {
        return; /* the node has acknowledged it again; the message itself was handled */
    }
    if (!fa_envelope_parse(dg->body, dg->body_len, &e))
    {
        c->stats.ignored++;
        return;
    }
    c->stats.messages_in++;
    switch (e.type)
    {
    case FA_TYPE_CONFIRM:
        /* 07D2 <our word> <server conversation> 6400. A repeated confirm changes nothing. */
        if (c->state != FA_CLIENT_CONNECTING || e.conversation != c->cfg.word)
        {
            c->stats.ignored++;
            return;
        }
        c->server_conversation = (uint16_t)((dg->body[4] << 8u) | dg->body[5]);
        XMSG_LOG(XMSG_LOG_COSMOS, XMSG_LOG_INFO,
                 "fa client: connected, server conversation 0x%04X, port 0x%04X",
                 (unsigned)c->server_conversation, (unsigned)c->base.peer_port);
        c->state = FA_CLIENT_RUNNING;
        c->step = FA_STEP_RESERVE;
        c->wait_since_ms = now_ms;
        if (c->cancelled)
        {
            /* Nothing reserved yet: only the session to end (unverified, see the header). */
            c->step = FA_STEP_RELEASE;
            next_step(c, now_ms);
            break;
        }
        enqueue_request(c, now_ms);
        break;
    case FA_TYPE_SHORT_ACK:
        break; /* the server has our request; its reply follows */
    case FA_TYPE_REQUEST:
        if (c->state != FA_CLIENT_RUNNING)
        {
            c->stats.ignored++;
            return;
        }
        c->wait_since_ms = now_ms;
        if (dg->body_len == FA_DATA_MESSAGE_SIZE)
        {
            on_data(c, dg->body, dg->body_len, now_ms);
        }
        else
        {
            on_reply(c, dg->body, dg->body_len, now_ms);
        }
        break;
    case FA_TYPE_CLOSE:
        if (c->state == FA_CLIENT_FINISHING)
        {
            finish_with(c, FA_CLIENT_DONE, (c->sintran_error != 0u) ? FA_CLIENT_EREFUSED : 0,
                        now_ms);
            return;
        }
        XMSG_LOG(XMSG_LOG_COSMOS, XMSG_LOG_WARN, "fa client: server closed the session early");
        fail(c, XMSG_EPROTO, now_ms);
        break;
    default:
        c->stats.ignored++;
        XMSG_LOG(XMSG_LOG_COSMOS, XMSG_LOG_DEBUG, "fa client: message type 0x%04X ignored",
                 (unsigned)e.type);
        break;
    }
}

static int client_poll_tx(XmsgSession *s, uint32_t now_ms)
{
    FaClient *c = (FaClient *)s;
    int rc;

    if (c->inflight.kind != FA_SEND_NONE || c->q_count == 0u)
    {
        update_wants_tx(c);
        return 0;
    }
    rc = send_item(c, &c->queue[c->q_head], now_ms);
    if (rc == XMSG_EAGAIN || rc == XMSG_ESTATE)
    {
        return 0; /* window full or link settling: next poll */
    }
    if (rc != XMSG_OK)
    {
        fail(c, rc, now_ms);
        return 0;
    }
    c->q_head = (uint8_t)((c->q_head + 1u) % FA_CLIENT_QUEUE);
    c->q_count--;
    update_wants_tx(c);
    return 1;
}

static void client_on_delivered(XmsgSession *s, uint16_t flags1)
{
    FaClient *c = (FaClient *)s;

    if (c->inflight.kind == FA_SEND_NONE || flags1 != c->inflight_flags1)
    {
        return;
    }
    c->inflight.kind = FA_SEND_NONE;
    c->xense_tries = 0;
    update_wants_tx(c);
}

/* Put the frame in flight back at the head of the queue, numbered again by the node at commit
 * but with the same FA counter and sequence. */
static void requeue_inflight(FaClient *c)
{
    c->q_head = (uint8_t)((c->q_head + FA_CLIENT_QUEUE - 1u) % FA_CLIENT_QUEUE);
    c->queue[c->q_head] = c->inflight;
    c->q_count++;
    c->inflight.kind = FA_SEND_NONE;
    update_wants_tx(c);
}

static void client_on_error(XmsgSession *s, int16_t code, uint16_t flags1)
{
    FaClient *c = (FaClient *)s;

    if (c->inflight.kind == FA_SEND_NONE || flags1 != c->inflight_flags1)
    {
        return;
    }
    if (code == XMSG_XE_XENSE && c->xense_tries < FA_CLIENT_XENSE_TRIES)
    {
        c->xense_tries++;
        c->stats.xense_resends++;
        requeue_inflight(c);
        return;
    }
    XMSG_LOG(XMSG_LOG_COSMOS, XMSG_LOG_ERROR, "fa client frame 0x%04X refused: XMSG error %d",
             flags1, (int)code);
    fail(c, code, c->inflight_since_ms);
}

static void client_on_peer_restart(XmsgSession *s)
{
    FaClient *c = (FaClient *)s;

    if (c->inflight.kind == FA_SEND_NONE)
    {
        return;
    }
    XMSG_LOG(XMSG_LOG_COSMOS, XMSG_LOG_INFO, "server restarted its count; frame 0x%04X again",
             c->inflight_flags1);
    c->stats.restart_resends++;
    requeue_inflight(c);
}

static void client_tick(XmsgSession *s, uint32_t now_ms)
{
    FaClient *c = (FaClient *)s;

    if (c->inflight.kind == FA_SEND_LETTER && !c->resync_tried &&
        (uint32_t)(now_ms - c->inflight_since_ms) >= c->cfg.ack_timeout_ms)
    {
        /* As the TAD asker: the server may hold a count for us that is ahead of ours. Ask it to
         * start again at 0 once; the letter is sent again from client_on_peer_restart. */
        c->resync_tried = true;
        XMSG_LOG(XMSG_LOG_COSMOS, XMSG_LOG_WARN, "connect letter not acknowledged: resync");
        if (xmsg_node_resync(c->base.node, c->base.peer_node, now_ms) == XMSG_OK)
        {
            c->inflight_since_ms = now_ms;
            return;
        }
    }
    if (c->inflight.kind != FA_SEND_NONE &&
        (uint32_t)(now_ms - c->inflight_since_ms) >= c->cfg.ack_timeout_ms)
    {
        XMSG_LOG(XMSG_LOG_COSMOS, XMSG_LOG_ERROR, "fa client frame 0x%04X not acknowledged",
                 c->inflight_flags1);
        fail(c, XMSG_ETIMEDOUT, now_ms);
        return;
    }
    if ((c->state == FA_CLIENT_CONNECTING || c->state == FA_CLIENT_RUNNING ||
         c->state == FA_CLIENT_FINISHING) &&
        c->inflight.kind == FA_SEND_NONE && c->q_count == 0u &&
        (uint32_t)(now_ms - c->wait_since_ms) >= c->cfg.reply_timeout_ms)
    {
        XMSG_LOG(XMSG_LOG_COSMOS, XMSG_LOG_ERROR, "fa client: no answer from the server in %u ms",
                 (unsigned)c->cfg.reply_timeout_ms);
        fail(c, XMSG_ETIMEDOUT, now_ms);
    }
}

static const XmsgSessionOps s_fa_client_ops = {
    client_on_datagram, client_poll_tx,     client_tick,
    client_on_delivered, client_on_error, client_on_peer_restart,
};

/* ------------------------------------------------------------------------------------------ */
/* Public                                                                                     */
/* ------------------------------------------------------------------------------------------ */

void fa_client_config_default(FaClientConfig *cfg)
{
    if (cfg == NULL)
    {
        return;
    }
    memset(cfg, 0, sizeof *cfg);
    cfg->word = 0x0002u;
    memcpy(cfg->asker, "BAK04", 6);
    cfg->ack_timeout_ms = 10000u;
    cfg->reply_timeout_ms = 30000u;
}

int fa_client_init(FaClient *c, const FaClientConfig *cfg, const FaClientCallbacks *cb)
{
    if (c == NULL)
    {
        return XMSG_EINVAL;
    }
    memset(c, 0, sizeof *c);
    if (cfg != NULL)
    {
        c->cfg = *cfg;
    }
    else
    {
        fa_client_config_default(&c->cfg);
    }
    if (cb != NULL)
    {
        c->cb = *cb;
    }
    c->base.ops = &s_fa_client_ops;
    c->state = FA_CLIENT_IDLE;
    return XMSG_OK;
}

/* Copy a NUL-terminated name into a fixed field. */
static bool copy_name(char *dst, size_t size, const char *src)
{
    size_t len;

    if (src == NULL)
    {
        return false;
    }
    len = strlen(src);
    if (len >= size)
    {
        return false;
    }
    memcpy(dst, src, len + 1u);
    return true;
}

/* What start() is asked to do. */
typedef enum FaClientJob
{
    FA_JOB_READ = 0,
    FA_JOB_LIST,
    FA_JOB_WRITE
} FaClientJob;

static int start(FaClient *c, XmsgNode *node, uint16_t peer_node, uint16_t peer_system,
                 const char *local_user, const char *remote_user, const char *password,
                 const char *spec, FaClientJob job, uint32_t length, uint32_t now_ms)
{
    FaClientConfig cfg;
    FaClientCallbacks cb;
    int rc;
    unsigned v;
    size_t nd = 0;
    char digits[6];
    bool listing;

    if (c == NULL || node == NULL || peer_node == 0u || peer_system == 0u)
    {
        return XMSG_EINVAL;
    }
    if (c->state != FA_CLIENT_IDLE && c->state != FA_CLIENT_DONE &&
        c->state != FA_CLIENT_FAILED)
    {
        return XMSG_ESTATE;
    }
    if (job == FA_JOB_WRITE && (length == 0u || c->cb.source == NULL))
    {
        return XMSG_EINVAL;
    }
    cfg = c->cfg;
    cb = c->cb;
    (void)fa_client_init(c, &cfg, &cb);
    listing = (job == FA_JOB_LIST);
    c->writing = (job == FA_JOB_WRITE);
    if (c->writing)
    {
        c->length = length;
        c->blocks = (length + FA_BLOCK_SIZE - 1u) / FA_BLOCK_SIZE;
    }
    if (!copy_name(c->local_user, sizeof c->local_user, local_user) ||
        !copy_name(c->remote_user, sizeof c->remote_user, remote_user) ||
        !copy_name(c->spec, sizeof c->spec, spec) || (!listing && c->spec[0] == '\0'))
    {
        return XMSG_EINVAL;
    }
    c->listing = listing;
    if (listing)
    {
        if (!fa_build_walk_block(c->walk_block, c->remote_user, c->spec))
        {
            return XMSG_EINVAL;
        }
        c->cursor = FA_WALK_FIRST_CURSOR;
    }
    c->password = sintran_password_fold(password);
    /* "D" and the system number in decimal, as SINTRAN names a system in a letter. */
    v = peer_system;
    do
    {
        digits[nd++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);
    c->system_name[0] = 'D';
    for (size_t i = 0; i < nd; i++)
    {
        c->system_name[1u + i] = digits[nd - 1u - i];
    }
    c->system_name[1u + nd] = '\0';

    c->base.peer_node = peer_node;
    c->base.peer_system = peer_system;
    c->base.peer_port = 0; /* learned from the confirm, which comes from the session port */
    rc = xmsg_node_attach(node, &c->base);
    if (rc != XMSG_OK)
    {
        return rc;
    }
    c->state = FA_CLIENT_CONNECTING;
    c->wait_since_ms = now_ms;
    XMSG_LOG(XMSG_LOG_COSMOS, XMSG_LOG_INFO, "fa client: %s \"%s\" on %s (node %u) as %s",
             listing ? "list" : (c->writing ? "write" : "read"), c->spec, c->system_name,
             (unsigned)peer_node,
             c->remote_user);
    enqueue(c, FA_SEND_LETTER, 0, 0, now_ms);
    return XMSG_OK;
}

int fa_client_read(FaClient *c, XmsgNode *node, uint16_t peer_node, uint16_t peer_system,
                   const char *local_user, const char *remote_user, const char *password,
                   const char *spec, uint32_t now_ms)
{
    return start(c, node, peer_node, peer_system, local_user, remote_user, password, spec,
                 FA_JOB_READ, 0, now_ms);
}

int fa_client_write(FaClient *c, XmsgNode *node, uint16_t peer_node, uint16_t peer_system,
                    const char *local_user, const char *remote_user, const char *password,
                    const char *spec, uint32_t length, FaWriteMode mode, uint32_t now_ms)
{
    int rc;

    if (mode != FA_WRITE_CREATE && mode != FA_WRITE_OVERWRITE)
    {
        return XMSG_EINVAL;
    }
    rc = start(c, node, peer_node, peer_system, local_user, remote_user, password, spec,
               FA_JOB_WRITE, length, now_ms);
    if (rc == XMSG_OK)
    {
        c->create = (mode == FA_WRITE_CREATE); /* read when the open is built, after the confirm */
    }
    return rc;
}

int fa_client_list(FaClient *c, XmsgNode *node, uint16_t peer_node, uint16_t peer_system,
                   const char *local_user, const char *remote_user, const char *password,
                   const char *filter, uint32_t now_ms)
{
    return start(c, node, peer_node, peer_system, local_user, remote_user, password, filter,
                 FA_JOB_LIST, 0, now_ms);
}

FaClientState fa_client_state(const FaClient *c)
{
    return (c != NULL) ? c->state : FA_CLIENT_FAILED;
}

int fa_client_cancel(FaClient *c, uint32_t now_ms)
{
    (void)now_ms; /* the teardown starts when the request in flight has been answered */
    if (c == NULL)
    {
        return XMSG_EINVAL;
    }
    if ((c->state != FA_CLIENT_CONNECTING && c->state != FA_CLIENT_RUNNING) || c->cancelled)
    {
        return XMSG_OK; /* nothing running, or FINISHING: all was delivered, only the close
                           is awaited, and it ends as it would have */
    }
    XMSG_LOG(XMSG_LOG_COSMOS, XMSG_LOG_INFO, "fa client: cancelled in %s, step %d",
             fa_client_state_name(c->state), (int)c->step);
    c->cancelled = true;
    return XMSG_OK;
}

const char *fa_client_state_name(FaClientState st)
{
    switch (st)
    {
    case FA_CLIENT_IDLE:
        return "idle";
    case FA_CLIENT_CONNECTING:
        return "connecting";
    case FA_CLIENT_RUNNING:
        return "running";
    case FA_CLIENT_FINISHING:
        return "finishing";
    case FA_CLIENT_DONE:
        return "done";
    case FA_CLIENT_FAILED:
        return "failed";
    default:
        return "?";
    }
}
