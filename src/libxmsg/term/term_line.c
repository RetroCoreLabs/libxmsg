/*
 * term_line.c - a small line editor and the bridge's command words.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/term/term_line.h"

#include <string.h>

void term_line_init(TermLine *l, bool echo)
{
    if (l == NULL)
    {
        return;
    }
    memset(l, 0, sizeof *l);
    l->echo = echo;
}

void term_line_clear(TermLine *l)
{
    if (l == NULL)
    {
        return;
    }
    memset(l->text, 0, sizeof l->text);
    l->len = 0;
}

int term_line_byte(TermLine *l, uint8_t b, uint8_t echo[3], size_t *echo_len)
{
    bool was_cr;

    if (l == NULL || echo == NULL || echo_len == NULL)
    {
        return -1;
    }
    *echo_len = 0;
    was_cr = l->after_cr;
    l->after_cr = false;
    if (was_cr && (b == 0x0Au || b == 0x00u))
    {
        return 0; /* the second half of CR LF or CR NUL */
    }
    if (b == 0x0Du || b == 0x0Au)
    {
        l->after_cr = (b == 0x0Du);
        echo[0] = 0x0Du;
        echo[1] = 0x0Au;
        *echo_len = 2;
        l->text[l->len] = '\0';
        return 1;
    }
    if (b == 0x08u || b == 0x7Fu)
    {
        if (l->len > 0)
        {
            l->len--;
            l->text[l->len] = '\0';
            if (l->echo)
            {
                echo[0] = 0x08u;
                echo[1] = (uint8_t)' ';
                echo[2] = 0x08u;
                *echo_len = 3;
            }
        }
        return 0;
    }
    if (b < 0x20u || b > 0x7Eu || l->len >= TERM_LINE_MAX)
    {
        return 0;
    }
    l->text[l->len++] = (char)b;
    l->text[l->len] = '\0';
    if (l->echo)
    {
        echo[0] = b;
        *echo_len = 1;
    }
    return 0;
}

static char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* Copy the next blank-separated word, in upper case unless keep_case. Returns its length, or -1
 * when it does not fit in cap - 1. *p is moved past it. */
static int next_word(const char **p, char *out, size_t cap, bool keep_case)
{
    size_t n = 0;
    const char *s = *p;

    while (*s == ' ' || *s == '\t')
    {
        s++;
    }
    while (*s != '\0' && *s != ' ' && *s != '\t')
    {
        if (n + 1u >= cap)
        {
            return -1;
        }
        out[n++] = keep_case ? *s : upper(*s);
        s++;
    }
    out[n] = '\0';
    *p = s;
    return (int)n;
}

/* ADDUSER, DELUSER and RESETPW: one user name, case kept; ADDUSER may add ADMIN. Returns true
 * when `word` is one of them, with the result in *cmd (TERM_CMD_UNKNOWN for a bad form). */
static bool user_command(const char *word, const char **p, char *arg, size_t cap, TermCmd *cmd)
{
    char extra[8];
    int al;
    int el;
    bool is_add = strcmp(word, "ADDUSER") == 0;
    if (!is_add && strcmp(word, "DELUSER") != 0 && strcmp(word, "RESETPW") != 0)
    {
        return false;
    }
    *cmd = TERM_CMD_UNKNOWN;
    al = next_word(p, arg, cap, true);
    el = next_word(p, extra, sizeof extra, false);
    if (al <= 0 || el < 0 || (el > 0 && (!is_add || strcmp(extra, "ADMIN") != 0)) ||
        next_word(p, extra, sizeof extra, false) != 0)
    {
        arg[0] = '\0';
        return true;
    }
    if (is_add)
    {
        *cmd = (el > 0) ? TERM_CMD_ADDADMIN : TERM_CMD_ADDUSER;
    }
    else
    {
        *cmd = (word[0] == 'D') ? TERM_CMD_DELUSER : TERM_CMD_RESETPW;
    }
    return true;
}

TermCmd term_cmd_parse(const char *line, char *arg, size_t cap)
{
    static const struct
    {
        const char *word;
        TermCmd cmd;
    } k_words[] = {
        {"LIST", TERM_CMD_LIST}, {"HELP", TERM_CMD_HELP},     {"?", TERM_CMD_HELP},
        {"WHO", TERM_CMD_WHO},   {"INFO", TERM_CMD_INFO},     {"EXIT", TERM_CMD_EXIT},
        {"QUIT", TERM_CMD_EXIT}, {"LOGOUT", TERM_CMD_EXIT},     {"PASSWORD", TERM_CMD_PASSWORD},
        {"USERS", TERM_CMD_USERS}, {"NAMES", TERM_CMD_NAMES},
    };
    char word[16];
    char extra[2];
    const char *p = line;
    int wl;
    int al;
    TermCmd cmd = TERM_CMD_UNKNOWN;

    if (line == NULL || arg == NULL || cap == 0)
    {
        return TERM_CMD_UNKNOWN;
    }
    arg[0] = '\0';
    wl = next_word(&p, word, sizeof word, false);
    if (wl == 0)
    {
        return TERM_CMD_EMPTY;
    }
    if (wl < 0)
    {
        return TERM_CMD_UNKNOWN;
    }
    if (wl > 0 && user_command(word, &p, arg, cap, &cmd))
    {
        return cmd;
    }
    al = next_word(&p, arg, cap, false);
    if (al < 0 || next_word(&p, extra, sizeof extra, false) != 0)
    {
        arg[0] = '\0';
        return TERM_CMD_UNKNOWN; /* an argument too long, or more words than any command takes */
    }
    if (strcmp(word, "CONNECT") == 0)
    {
        return (al > 0) ? TERM_CMD_CONNECT : TERM_CMD_UNKNOWN;
    }
    if (strcmp(word, "NAMES") == 0)
    {
        return TERM_CMD_NAMES; /* the system is optional: none is the host we are linked to */
    }
    if (al > 0)
    {
        arg[0] = '\0';
        return TERM_CMD_UNKNOWN;
    }
    for (size_t i = 0; i < sizeof k_words / sizeof k_words[0]; i++)
    {
        if (strcmp(word, k_words[i].word) == 0)
        {
            return k_words[i].cmd;
        }
    }
    return TERM_CMD_UNKNOWN;
}
