/*
 * fa_text.c - the text forms SINTRAN prints for a file. Sources are in fa_text.h.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libcosmos/fa/fa_text.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* The five rights of one tier, bit 0 first. */
static const char s_letters[5] = {'r', 'w', 'a', 'c', 'd'};
static const char *const s_words[5] = {"READ", "WRITE", "APPEND", "COMMON", "DIRECTORY"};

static const char *const s_months_stat[12] = {"JANUARY",   "FEBRUARY", "MARCH",    "APRIL",
                                              "MAY",       "JUNE",     "JULY",     "AUGUST",
                                              "SEPTEMBER", "OCTOBER",  "NOVEMBER", "DECEMBER"};
static const char *const s_months_ftp[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/* snprintf, turned into "the length, or -1 when it did not fit". */
static int fitted(int n, size_t cap)
{
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

int fa_text_access_letters(uint16_t access, char *out, size_t cap)
{
    static const unsigned shifts[3] = {FA_ACCESS_OWN_SHIFT, FA_ACCESS_FRIEND_SHIFT,
                                       FA_ACCESS_PUBLIC_SHIFT};
    size_t at = 0;

    if (out == NULL || cap < FA_TEXT_ACCESS_LETTERS)
    {
        return -1;
    }
    for (size_t t = 0; t < 3u; t++)
    {
        for (unsigned b = 0; b < 5u; b++)
        {
            out[at++] = ((access >> (shifts[t] + b)) & 1u) != 0u ? s_letters[b] : '-';
        }
    }
    out[at] = '\0';
    return (int)at;
}

int fa_text_access_words(uint16_t access, unsigned shift, char *out, size_t cap)
{
    size_t len = 0;

    if (out == NULL || cap == 0u ||
        (shift != FA_ACCESS_OWN_SHIFT && shift != FA_ACCESS_FRIEND_SHIFT &&
         shift != FA_ACCESS_PUBLIC_SHIFT))
    {
        return -1;
    }
    out[0] = '\0';
    for (unsigned b = 0; b < 5u; b++)
    {
        if (((access >> (shift + b)) & 1u) != 0u)
        {
            int n = snprintf(out + len, cap - len, "%s%s", (len > 0u) ? ", " : "", s_words[b]);
            if (fitted(n, cap - len) < 0)
            {
                return -1;
            }
            len += (size_t)n;
        }
    }
    if (len == 0u)
    {
        return fitted(snprintf(out, cap, "NONE"), cap);
    }
    return (int)len;
}

int fa_text_date_stat(uint32_t raw, char *out, size_t cap)
{
    FaNdDate d;

    if (out == NULL || cap == 0u)
    {
        return -1;
    }
    out[0] = '\0';
    if (!fa_nd_date_split(raw, &d) || d.month < 1u || d.month > 12u)
    {
        return 0;
    }
    return fitted(snprintf(out, cap, "%02u.%02u.%02u  %s %u, %u", (unsigned)d.hour,
                           (unsigned)d.minute, (unsigned)d.second, s_months_stat[d.month - 1u],
                           (unsigned)d.day, (unsigned)d.year),
                  cap);
}

int fa_text_date_ftp(uint32_t raw, char *out, size_t cap)
{
    FaNdDate d;

    if (out == NULL || cap == 0u)
    {
        return -1;
    }
    if (!fa_nd_date_split(raw, &d) || d.month < 1u || d.month > 12u)
    {
        return fitted(snprintf(out, cap, "00 pre 50 00:00"), cap); /* measured, see the header */
    }
    return fitted(snprintf(out, cap, "%02u %s %02u %02u:%02u", (unsigned)d.day,
                           s_months_ftp[d.month - 1u], (unsigned)(d.year % 100u), (unsigned)d.hour,
                           (unsigned)d.minute),
                  cap);
}

uint16_t fa_text_version(const FaFileEntry *e)
{
    if (e == NULL || e->next_version != e->object_index)
    {
        return 0;
    }
    return 1;
}

int fa_text_name(const FaFileEntry *e, FaNameForm form, char *out, size_t cap)
{
    uint16_t v;

    if (e == NULL || out == NULL || cap == 0u)
    {
        return -1;
    }
    switch (form)
    {
    case FA_NAME_SINTRAN:
        return fitted(snprintf(out, cap, "%s:%s", e->name, e->type), cap);
    case FA_NAME_FTP:
        return fitted(snprintf(out, cap, "%s.%s", e->name, e->type), cap);
    case FA_NAME_SINTRAN_VERSION:
        /* fall through */
    case FA_NAME_FTP_VERSION:
        v = fa_text_version(e);
        if (v == 0u)
        {
            return -1;
        }
        return fitted(snprintf(out, cap, (form == FA_NAME_FTP_VERSION) ? "%s.%s-%u" : "%s:%s;%u",
                               e->name, e->type, (unsigned)v),
                      cap);
    default:
        return -1;
    }
}

int fa_text_name_from_ftp(const char *ftp, char *out, size_t cap)
{
    size_t len;
    const char *dot;
    const char *dash;
    bool version = true;

    if (ftp == NULL || out == NULL || cap == 0u)
    {
        return -1;
    }
    len = strlen(ftp);
    if (len >= cap)
    {
        return -1;
    }
    memcpy(out, ftp, len + 1u);
    dot = strrchr(ftp, '.');
    if (strchr(ftp, ':') != NULL || dot == NULL)
    {
        return (int)len;
    }
    out[dot - ftp] = ':';
    dash = strrchr(dot, '-');
    if (dash == NULL || dash[1] == '\0')
    {
        return (int)len;
    }
    for (const char *p = dash + 1; *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9')
        {
            version = false;
            break;
        }
    }
    if (version)
    {
        len = (size_t)(dash - ftp);
        out[len] = '\0';
    }
    return (int)len;
}
