/*
 * fa_replies.c - reading what a COSMOS file-access server sends back.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libcosmos/fa/fa_replies.h"

#include <string.h>

#include "libcosmos/qform/qform.h"

bool fa_reply_parse(const uint8_t *body, size_t len, FaReply *r)
{
    QformItem it;
    uint32_t v;

    if (body == NULL || r == NULL || !fa_envelope_parse(body, len, &r->env))
    {
        return false;
    }
    /* 92 op 92 seq: six bytes after the envelope. */
    if (len < FA_ENVELOPE_SIZE + 6u || body[8] != QFORM_TAG_INT16 || body[11] != QFORM_TAG_INT16)
    {
        return false;
    }
    r->op = (uint16_t)((body[9] << 8u) | body[10]);
    r->seq = (uint16_t)((body[12] << 8u) | body[13]);
    r->fields = body + FA_ENVELOPE_SIZE + 6u;
    r->fields_len = len - FA_ENVELOPE_SIZE - 6u;
    r->refused = false;
    r->error = 0;
    if (qform_find_field(r->fields, r->fields_len, 1, &it) && it.tag == QFORM_TAG_TYPED16 &&
        qform_item_uint(&it, &v))
    {
        r->refused = true;
        r->error = (uint16_t)v;
    }
    return true;
}

bool fa_reply_open_info(const FaReply *r, uint16_t *file_number, uint32_t *length)
{
    QformItem it;
    uint32_t v;

    if (r == NULL || file_number == NULL || length == NULL)
    {
        return false;
    }
    if (!qform_find_field(r->fields, r->fields_len, 2, &it) || !qform_item_uint(&it, &v))
    {
        return false;
    }
    *file_number = (uint16_t)v;
    if (!qform_find_field(r->fields, r->fields_len, 3, &it) || it.tag != QFORM_TAG_TYPED32 ||
        !qform_item_uint(&it, &v))
    {
        return false;
    }
    *length = v;
    return true;
}

bool fa_reply_file_number(const FaReply *r, uint16_t *file_number)
{
    QformItem it;
    uint32_t v;

    if (r == NULL || file_number == NULL ||
        !qform_find_field(r->fields, r->fields_len, 2, &it) || !qform_item_uint(&it, &v))
    {
        return false;
    }
    *file_number = (uint16_t)v;
    return true;
}

const uint8_t *fa_reply_object_entry(const FaReply *r)
{
    QformReader rd;
    QformItem it;

    if (r == NULL || !qform_find_field(r->fields, r->fields_len, 2, &it) ||
        it.cls != QFORM_CLASS_CONSTRUCTED)
    {
        return NULL;
    }
    qform_reader_init(&rd, it.value, it.len);
    if (qform_reader_next(&rd, &it) && it.cls == QFORM_CLASS_STRING &&
        it.len == FA_OBJECT_ENTRY_SIZE)
    {
        return it.value;
    }
    return NULL;
}

const char *fa_error_name(uint16_t error)
{
    switch (error)
    {
    case FA_ERR_NO_SUCH_FILE:
        return "no such file";
    case FA_ERR_WRONG_PASSWORD:
        return "wrong password";
    case FA_ERR_END_OF_DIRECTORY:
        return "end of directory";
    default:
        return "";
    }
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8u) | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24u) | ((uint32_t)p[1] << 16u) | ((uint32_t)p[2] << 8u) | p[3];
}

/* Copy a SINTRAN name field: up to its apostrophe (0x27) or NUL, or the whole field. */
static void copy_field(char *out, size_t cap, const uint8_t *field, size_t size)
{
    size_t n = 0;

    while (n < size && n + 1u < cap && field[n] != 0x27u && field[n] != 0u)
    {
        out[n] = (char)field[n];
        n++;
    }
    out[n] = '\0';
}

void fa_entry_parse(const uint8_t *raw, FaFileEntry *e)
{
    uint32_t bytes_minus_one;

    if (raw == NULL || e == NULL)
    {
        return;
    }
    memset(e, 0, sizeof *e);
    e->header = be16(raw);
    copy_field(e->name, sizeof e->name, raw + 2, 16u);
    copy_field(e->type, sizeof e->type, raw + 18, 4u);
    e->next_version = be16(raw + 22);
    e->prev_version = be16(raw + 24);
    e->access = be16(raw + 26);
    e->attributes = be16(raw + 28);
    e->device = be16(raw + 30);
    e->owner_index = raw[34];
    e->slot = raw[35];
    e->open_now = be16(raw + 36);
    e->open_total = be16(raw + 38);
    e->created = be32(raw + 40);
    e->last_read = be32(raw + 44);
    e->last_write = be32(raw + 48);
    e->pages = be32(raw + 52);
    /* Stored as the length minus one; FFFFFFFF (device files) means no length at all. */
    bytes_minus_one = be32(raw + 56);
    e->has_bytes = (bytes_minus_one != 0xFFFFFFFFu);
    e->bytes = e->has_bytes ? bytes_minus_one + 1u : 0u;
    e->file_pointer = be32(raw + 60);
}

bool fa_nd_date_split(uint32_t raw, FaNdDate *d)
{
    if (d == NULL || raw == 0u)
    {
        return false;
    }
    d->year = (uint16_t)(1950u + ((raw >> 26u) & 0x3Fu));
    d->month = (uint8_t)((raw >> 22u) & 0x0Fu);
    d->day = (uint8_t)((raw >> 17u) & 0x1Fu);
    d->hour = (uint8_t)((raw >> 12u) & 0x1Fu);
    d->minute = (uint8_t)((raw >> 6u) & 0x3Fu);
    d->second = (uint8_t)(raw & 0x3Fu);
    return d->month >= 1u && d->month <= 12u && d->day >= 1u && d->day <= 31u &&
           d->hour <= 23u && d->minute <= 59u && d->second <= 59u;
}

bool fa_reply_walk_entry(const FaReply *r, uint16_t *user_index, uint16_t *object_index,
                         const uint8_t **entry)
{
    QformReader rd;
    QformItem it;
    uint32_t v[3];
    int n = 0;

    if (r == NULL || user_index == NULL || object_index == NULL || entry == NULL ||
        !qform_find_field(r->fields, r->fields_len, 2, &it) || it.cls != QFORM_CLASS_CONSTRUCTED)
    {
        return false;
    }
    qform_reader_init(&rd, it.value, it.len);
    while (qform_reader_next(&rd, &it))
    {
        if (n < 3 && it.tag == QFORM_TAG_TYPED16 && qform_item_uint(&it, &v[n]))
        {
            n++;
            continue;
        }
        if (n == 3 && it.cls == QFORM_CLASS_STRING && it.len == FA_OBJECT_ENTRY_SIZE)
        {
            *user_index = (uint16_t)v[0];
            *object_index = (uint16_t)v[1];
            *entry = it.value;
            return true;
        }
        return false;
    }
    return false;
}

bool fa_reply_entry_name(const FaReply *r, size_t offset, char *out, size_t cap)
{
    QformReader rd;
    QformItem it;

    if (r == NULL || out == NULL || cap == 0u ||
        !qform_find_field(r->fields, r->fields_len, 2, &it) || it.cls != QFORM_CLASS_CONSTRUCTED)
    {
        return false;
    }
    qform_reader_init(&rd, it.value, it.len);
    if (!qform_reader_next(&rd, &it) || it.cls != QFORM_CLASS_STRING || it.len <= offset)
    {
        return false;
    }
    copy_field(out, cap, it.value + offset, it.len - offset);
    return true;
}
