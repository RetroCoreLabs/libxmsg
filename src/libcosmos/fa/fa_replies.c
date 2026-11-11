/*
 * fa_replies.c - reading what a COSMOS file-access server sends back.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libcosmos/fa/fa_replies.h"

#include "libcosmos/qform/qform.h"

#define FA_OBJECT_ENTRY_SIZE 64u

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
