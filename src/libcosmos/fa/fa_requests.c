/*
 * fa_requests.c - building the messages a COSMOS file-access client sends.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libcosmos/fa/fa_requests.h"

#include <string.h>

#include "libcosmos/qform/qform.h"

/* MEASURED constants of the reserve (all five captured reserves; RetroIO FaWriteRequests.cs).
 * Their meaning is not known; they are sent as captured. */
#define FA_RESERVE_FIELD1 0x07D0u
#define FA_USER_SLOT      16u

/* The XROUT letter head: serial, service XSLET (65), payload length. */
#define FA_LETTER_SERIAL 0x1Bu
#define FA_LETTER_XSLET  0x41u
#define FA_SERVICE_NAME  "*FA-SERVER"

/* Write the envelope, 92 op and 92 seq. */
static void put_head(QformWriter *w, const FaEnvelope *env, uint16_t op, uint16_t seq)
{
    uint8_t head[FA_ENVELOPE_SIZE];

    fa_envelope_write(head, env);
    qform_put_raw(w, head, sizeof head);
    qform_put_tag16(w, QFORM_TAG_INT16, op);
    qform_put_tag16(w, QFORM_TAG_INT16, seq);
}

/* End the field list and pad to even. Returns the length or 0 if anything failed. */
static size_t finish(QformWriter *w)
{
    qform_put_selector(w, QFORM_SELECTOR_END);
    if ((w->len & 1u) != 0u)
    {
        qform_put_u8(w, 0);
    }
    return w->failed ? 0u : w->len;
}

/* 8C 80 <len>: a constructed value with its length in the marker form, as the real client
 * writes it on its walk requests. The length is filled in by end_marked(). */
static size_t begin_marked(QformWriter *w)
{
    size_t at = w->len;
    qform_put_u8(w, QFORM_TAG_CONSTRUCTED);
    qform_put_u8(w, 0x80u);
    qform_put_u8(w, 0);
    return at;
}

static void end_marked(QformWriter *w, size_t at)
{
    if (w->failed)
    {
        return;
    }
    w->buf[at + 2u] = (uint8_t)(w->len - at - 3u);
}

/* A user name in a 16-byte slot: the name, an apostrophe, zeros. */
static void put_user_slot(QformWriter *w, const char *name, size_t len)
{
    uint8_t slot[FA_USER_SLOT];

    memset(slot, 0, sizeof slot);
    memcpy(slot, name, len);
    slot[len] = (uint8_t)'\'';
    qform_put_string(w, slot, sizeof slot);
}

size_t fa_build_connect_letter(uint8_t *out, size_t cap, const char *system, uint16_t word)
{
    static const uint8_t trailer_head[4] = {0x07, 0xE2, 0x00, 0x00};
    static const uint8_t trailer_tail[6] = {0x64, 0x00, 0xA2, 0x00, 0xFF, 0x00};
    const size_t name_len = sizeof FA_SERVICE_NAME - 1u;
    size_t sys_len;
    size_t payload;
    QformWriter w;

    if (out == NULL || system == NULL)
    {
        return 0;
    }
    sys_len = strlen(system);
    /* An odd system name would need a fill byte before the trailer; no capture shows one. */
    if (sys_len == 0u || sys_len > 254u || (sys_len & 1u) != 0u)
    {
        return 0;
    }
    payload = 2u + name_len + 2u + sys_len;
    qform_writer_init(&w, out, cap);
    qform_put_u8(&w, FA_LETTER_SERIAL);
    qform_put_u8(&w, FA_LETTER_XSLET);
    qform_put_u8(&w, (uint8_t)(payload >> 8u));
    qform_put_u8(&w, (uint8_t)(payload & 0xFFu));
    qform_put_u8(&w, 0xFFu); /* parameter 1, a string: the service */
    qform_put_u8(&w, (uint8_t)name_len);
    qform_put_raw(&w, (const uint8_t *)FA_SERVICE_NAME, name_len);
    qform_put_u8(&w, 0xFEu); /* parameter 2, a string: the system */
    qform_put_u8(&w, (uint8_t)sys_len);
    qform_put_raw(&w, (const uint8_t *)system, sys_len);
    /* The trailer after the two string parameters. Only the word is understood (the server
     * echoes it); the rest is sent exactly as captured. */
    qform_put_raw(&w, trailer_head, sizeof trailer_head);
    qform_put_u8(&w, (uint8_t)(word >> 8u));
    qform_put_u8(&w, (uint8_t)(word & 0xFFu));
    qform_put_raw(&w, trailer_tail, sizeof trailer_tail);
    return w.failed ? 0u : w.len;
}

size_t fa_build_reserve(uint8_t *out, size_t cap, const FaEnvelope *env, uint16_t seq,
                        const char *asker, const char *local_user, const char *remote_user,
                        uint16_t password)
{
    uint8_t text[15];
    uint8_t slot[FA_USER_SLOT];
    size_t asker_len;
    size_t local_len;
    size_t remote_len;
    size_t at;
    QformWriter w;

    if (out == NULL || env == NULL || asker == NULL || local_user == NULL || remote_user == NULL)
    {
        return 0;
    }
    asker_len = strlen(asker);
    local_len = strlen(local_user);
    remote_len = strlen(remote_user);
    /* Field 3 is "<asker>  <user>" in a compact string (15 bytes at most); the remote user
     * needs room for its apostrophe in 16. */
    if (asker_len + 2u + local_len > sizeof text || remote_len == 0u || remote_len >= FA_USER_SLOT)
    {
        return 0;
    }
    memcpy(text, asker, asker_len);
    text[asker_len] = (uint8_t)' ';
    text[asker_len + 1u] = (uint8_t)' ';
    memcpy(text + asker_len + 2u, local_user, local_len);

    qform_writer_init(&w, out, cap);
    put_head(&w, env, FA_OP_RESERVE, seq);
    qform_put_selector(&w, 1);
    qform_put_tag16(&w, QFORM_TAG_TYPED16, FA_RESERVE_FIELD1);
    qform_put_selector(&w, 2);
    at = qform_begin_constructed(&w);
    qform_put_tag16(&w, QFORM_TAG_INT16, 1);
    qform_put_tag16(&w, QFORM_TAG_INT16, 1);
    qform_end_constructed(&w, at);
    qform_put_selector(&w, 3);
    qform_put_string(&w, text, asker_len + 2u + local_len);
    qform_put_selector(&w, 4);
    at = qform_begin_constructed(&w);
    put_user_slot(&w, remote_user, remote_len);
    qform_put_u8(&w, 0xE1u); /* MEASURED E1 80 between the user and the password; meaning unknown */
    qform_put_u8(&w, 0x80u);
    memset(slot, 0, sizeof slot);
    slot[0] = (uint8_t)(password >> 8u);
    slot[1] = (uint8_t)(password & 0xFFu);
    qform_put_string(&w, slot, sizeof slot);
    memset(slot, 0, sizeof slot);
    qform_put_string(&w, slot, sizeof slot); /* never seen anything but zeros */
    qform_end_constructed(&w, at);
    return finish(&w);
}

size_t fa_build_open_read(uint8_t *out, size_t cap, const FaEnvelope *env, uint16_t seq,
                          const char *spec)
{
    uint8_t text[255];
    size_t len;
    QformWriter w;

    if (out == NULL || env == NULL || spec == NULL)
    {
        return 0;
    }
    len = strlen(spec);
    if (len == 0u || len + 2u > sizeof text)
    {
        return 0;
    }
    memcpy(text, spec, len);
    text[len] = (uint8_t)'\'';
    text[len + 1u] = (uint8_t)'.';
    qform_writer_init(&w, out, cap);
    put_head(&w, env, FA_OP_OPEN, seq);
    qform_put_selector(&w, 2);
    qform_put_string(&w, text, len + 2u);
    return finish(&w);
}

size_t fa_build_open_write(uint8_t *out, size_t cap, const FaEnvelope *env, uint16_t seq,
                           const char *spec, bool create)
{
    uint8_t text[255];
    size_t len;
    size_t at = 0;
    QformWriter w;

    if (out == NULL || env == NULL || spec == NULL)
    {
        return 0;
    }
    len = strlen(spec);
    if (len == 0u || len + 4u > sizeof text)
    {
        return 0;
    }
    if (create)
    {
        text[at++] = (uint8_t)'"';
    }
    memcpy(text + at, spec, len);
    at += len;
    if (create)
    {
        text[at++] = (uint8_t)'"';
    }
    text[at++] = (uint8_t)'\'';
    text[at++] = (uint8_t)'W';
    qform_writer_init(&w, out, cap);
    put_head(&w, env, FA_OP_OPEN, seq);
    qform_put_selector(&w, 2);
    qform_put_string(&w, text, at);
    qform_put_selector(&w, 3);
    qform_put_tag16(&w, QFORM_TAG_INT16, 1);
    return finish(&w);
}

size_t fa_build_write_block(uint8_t *out, size_t cap, const FaEnvelope *env, uint16_t seq,
                            uint32_t block)
{
    QformWriter w;

    if (out == NULL || env == NULL)
    {
        return 0;
    }
    qform_writer_init(&w, out, cap);
    put_head(&w, env, FA_OP_WRITE, seq);
    qform_put_selector(&w, 1);
    qform_put_typed32(&w, block);
    return finish(&w);
}

size_t fa_build_set_end_of_file(uint8_t *out, size_t cap, const FaEnvelope *env, uint16_t seq,
                                uint32_t length)
{
    QformWriter w;
    size_t at;

    if (out == NULL || env == NULL || length == 0u)
    {
        return 0;
    }
    qform_writer_init(&w, out, cap);
    put_head(&w, env, FA_OP_SPECIAL, seq);
    qform_put_selector(&w, 1);
    qform_put_tag16(&w, QFORM_TAG_INT16, FA_SPECIAL_SET_END_OF_FILE);
    qform_put_selector(&w, 2);
    at = begin_marked(&w);
    qform_put_typed32(&w, length - 1u); /* the last byte's index */
    end_marked(&w, at);
    return finish(&w);
}

size_t fa_build_data_message(uint8_t *out, size_t cap, const FaEnvelope *env, const uint8_t *bytes)
{
    if (out == NULL || env == NULL || bytes == NULL || cap < FA_DATA_MESSAGE_SIZE)
    {
        return 0;
    }
    fa_envelope_write(out, env);
    memcpy(out + FA_ENVELOPE_SIZE, bytes, FA_DATA_BYTES);
    return FA_DATA_MESSAGE_SIZE;
}

size_t fa_build_set_block_size(uint8_t *out, size_t cap, const FaEnvelope *env, uint16_t seq,
                               uint16_t size)
{
    QformWriter w;

    if (out == NULL || env == NULL)
    {
        return 0;
    }
    qform_writer_init(&w, out, cap);
    put_head(&w, env, FA_OP_SET_BLOCK_SIZE, seq);
    qform_put_selector(&w, 1);
    qform_put_tag16(&w, QFORM_TAG_TYPED16, size);
    return finish(&w);
}

size_t fa_build_file_info(uint8_t *out, size_t cap, const FaEnvelope *env, uint16_t seq)
{
    QformWriter w;

    if (out == NULL || env == NULL)
    {
        return 0;
    }
    qform_writer_init(&w, out, cap);
    put_head(&w, env, FA_OP_SPECIAL, seq);
    qform_put_selector(&w, 1);
    qform_put_tag16(&w, QFORM_TAG_INT16, FA_SPECIAL_FILE_INFO);
    return finish(&w);
}

size_t fa_build_read_block(uint8_t *out, size_t cap, const FaEnvelope *env, uint16_t seq,
                           uint32_t block)
{
    QformWriter w;

    if (out == NULL || env == NULL)
    {
        return 0;
    }
    qform_writer_init(&w, out, cap);
    put_head(&w, env, FA_OP_READ, seq);
    qform_put_selector(&w, 1);
    qform_put_typed32(&w, block);
    return finish(&w);
}

bool fa_build_walk_block(uint8_t *block, const char *user, const char *spec)
{
    size_t ulen;
    size_t slen;
    size_t pos = 0;

    if (block == NULL || user == NULL || spec == NULL)
    {
        return false;
    }
    ulen = strlen(user);
    slen = strlen(spec);
    if (ulen == 0u || 2u + ulen + 2u * (slen + 1u) > FA_WALK_BLOCK_SIZE)
    {
        return false;
    }
    memset(block, 0, FA_WALK_BLOCK_SIZE);
    block[pos++] = (uint8_t)'(';
    memcpy(block + pos, user, ulen);
    pos += ulen;
    block[pos++] = (uint8_t)')';
    for (int copy = 0; copy < 2; copy++)
    {
        memcpy(block + pos, spec, slen);
        pos += slen;
        block[pos++] = (uint8_t)0x27; /* the SINTRAN terminator ' */
    }
    return true;
}

size_t fa_build_walk_next(uint8_t *out, size_t cap, const FaEnvelope *env, uint16_t seq,
                          const uint8_t *block, uint16_t user_index, uint16_t cursor)
{
    QformWriter w;
    size_t at;

    if (out == NULL || env == NULL || block == NULL)
    {
        return 0;
    }
    qform_writer_init(&w, out, cap);
    put_head(&w, env, FA_OP_SPECIAL, seq);
    qform_put_selector(&w, 1);
    qform_put_tag16(&w, QFORM_TAG_INT16, FA_SPECIAL_NEXT_ENTRY);
    qform_put_selector(&w, 2);
    at = begin_marked(&w);
    qform_put_string(&w, block, FA_WALK_BLOCK_SIZE);
    qform_put_tag16(&w, QFORM_TAG_TYPED16, user_index);
    qform_put_tag16(&w, QFORM_TAG_TYPED16, cursor);
    end_marked(&w, at);
    return finish(&w);
}

size_t fa_build_special_index(uint8_t *out, size_t cap, const FaEnvelope *env, uint16_t seq,
                              uint16_t sub, uint16_t index)
{
    QformWriter w;
    size_t at;

    if (out == NULL || env == NULL)
    {
        return 0;
    }
    qform_writer_init(&w, out, cap);
    put_head(&w, env, FA_OP_SPECIAL, seq);
    qform_put_selector(&w, 1);
    qform_put_tag16(&w, QFORM_TAG_INT16, sub);
    qform_put_selector(&w, 2);
    at = begin_marked(&w);
    qform_put_tag16(&w, QFORM_TAG_TYPED16, index);
    end_marked(&w, at);
    return finish(&w);
}

size_t fa_build_bare(uint8_t *out, size_t cap, const FaEnvelope *env, uint16_t op, uint16_t seq)
{
    QformWriter w;

    if (out == NULL || env == NULL)
    {
        return 0;
    }
    qform_writer_init(&w, out, cap);
    put_head(&w, env, op, seq);
    return finish(&w);
}

size_t fa_build_short_ack(uint8_t *out, size_t cap, uint16_t conversation, uint8_t counter)
{
    FaEnvelope e;

    if (out == NULL || cap < FA_ENVELOPE_SIZE)
    {
        return 0;
    }
    e.type = FA_TYPE_SHORT_ACK;
    e.conversation = conversation;
    e.counter = counter;
    e.token = FA_TOKEN_CLIENT_ACK;
    fa_envelope_write(out, &e);
    return FA_ENVELOPE_SIZE;
}

size_t fa_build_session_finished(uint8_t *out, size_t cap, uint16_t conversation, uint16_t word)
{
    if (out == NULL || cap < 10u)
    {
        return 0;
    }
    out[0] = (uint8_t)(FA_TYPE_SESSION_FINISHED >> 8u);
    out[1] = (uint8_t)(FA_TYPE_SESSION_FINISHED & 0xFFu);
    out[2] = (uint8_t)(conversation >> 8u);
    out[3] = (uint8_t)(conversation & 0xFFu);
    out[4] = (uint8_t)(word >> 8u);
    out[5] = (uint8_t)(word & 0xFFu);
    out[6] = 0x80u;
    out[7] = 0x00u;
    out[8] = 0x00u;
    out[9] = 0x00u;
    return 10u;
}
