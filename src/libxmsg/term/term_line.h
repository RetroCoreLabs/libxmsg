/*
 * term_line.h - a small line editor and the bridge's command words, for a telnet command line.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * The editor takes typed bytes one at a time and gives back what to echo and, at the end of a
 * line, the line. Backspace (0x08) and DEL (0x7F) take back the last character (echo BS SP BS).
 * CR ends a line; the LF or NUL a telnet client sends after CR (CR LF, CR NUL) is dropped. A
 * bare LF also ends a line. Other control bytes are ignored. Printable bytes beyond the buffer
 * are ignored. With echo off (a password) nothing is echoed except the CR LF at the end.
 *
 * Pure logic: no I/O.
 */
#ifndef XMSG_TERM_LINE_H
#define XMSG_TERM_LINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** The longest line kept, without its terminating zero. */
#define TERM_LINE_MAX 80u

/** Editor state. */
typedef struct TermLine
{
    char text[TERM_LINE_MAX + 1u]; /**< the line so far, zero-terminated */
    size_t len;
    bool echo;     /**< echo typed characters */
    bool after_cr; /**< the last byte was CR: a following LF or NUL is dropped */
} TermLine;

/**
 * @brief Start an empty line.
 * @param l    Editor.
 * @param echo Whether typed characters are echoed.
 */
void term_line_init(TermLine *l, bool echo);

/**
 * @brief Empty the line, wiping what was in it (it may have held a password).
 * @param l Editor.
 */
void term_line_clear(TermLine *l);

/**
 * @brief Feed one typed byte.
 * @param l        Editor.
 * @param b        The byte.
 * @param echo     Receives the bytes to echo (at most 3).
 * @param echo_len Receives their count.
 * @return 1 when the line is complete (in l->text; call term_line_clear() after use),
 *         0 when not, -1 for a NULL argument.
 */
int term_line_byte(TermLine *l, uint8_t b, uint8_t echo[3], size_t *echo_len);

/** The bridge's commands. */
typedef enum TermCmd
{
    TERM_CMD_EMPTY = 0, /**< nothing typed */
    TERM_CMD_UNKNOWN,
    TERM_CMD_CONNECT, /**< CONNECT <system> */
    TERM_CMD_LIST,    /**< LIST: the systems that can be reached */
    TERM_CMD_HELP,    /**< HELP or ? */
    TERM_CMD_WHO,     /**< WHO: the clients on the bridge */
    TERM_CMD_INFO,    /**< INFO: the link facts */
    TERM_CMD_EXIT     /**< EXIT, LOGOUT or QUIT */
} TermCmd;

/**
 * @brief Read a command line. Words are matched without regard to case; the argument is
 *        returned in upper case (SINTRAN system names are upper case).
 * @param line The line.
 * @param arg  Receives the first argument, zero-terminated ("" when none).
 * @param cap  Size of arg.
 * @return The command. CONNECT without an argument, or with an argument that does not fit,
 *         or any command with an argument it does not take, is TERM_CMD_UNKNOWN.
 */
TermCmd term_cmd_parse(const char *line, char *arg, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_TERM_LINE_H */
