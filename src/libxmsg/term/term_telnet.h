/*
 * term_telnet.h - the telnet part of a command line on TCP: option talk in, IAC doubling out.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Moved out of examples/host_tad_telnet so every telnet front end (the host tool, XMSGGW) uses
 * the same code. The application passes in the bytes it read from the socket and gets back the
 * data bytes the user typed, plus any answer bytes that must go back to the socket. Bytes for
 * the user go through term_telnet_escape(), which doubles 0xFF as telnet needs.
 * Pure logic: no I/O.
 *
 * The opening (term_telnet_open): we will echo and suppress go-ahead, and we ask the client to
 * suppress go-ahead (RFC 857, 858), so the client sends each key as typed and does not echo
 * itself. With `binary` set we also will send binary and ask the client for binary (RFC 856),
 * so 8-bit data and CR pass with no translation. Other options are refused. Answers follow the
 * loop rule of RFC 1143: only a CHANGE is answered, an answer that merely agrees changes nothing.
 *
 * Data: telnet commands are taken out of the stream; IAC IAC is the data byte 0xFF. While the
 * client is not in binary mode a CR is sent as CR NUL (RFC 854) and that NUL is not a typed
 * byte, so it is dropped. Every other byte is passed on as it is.
 */
#ifndef XMSG_TERM_TELNET_H
#define XMSG_TERM_TELNET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Telnet bytes (RFC 854) and the options used here. */
#define TERM_TN_IAC    0xFFu
#define TERM_TN_DONT   0xFEu
#define TERM_TN_DO     0xFDu
#define TERM_TN_WONT   0xFCu
#define TERM_TN_WILL   0xFBu
#define TERM_TN_SB     0xFAu
#define TERM_TN_SE     0xF0u
#define TERM_TN_BINARY 0x00u /**< RFC 856: 8-bit data, no CR translation */
#define TERM_TN_ECHO   0x01u /**< RFC 857: we echo, the client does not */
#define TERM_TN_SGA    0x03u /**< RFC 858: no go-ahead, i.e. character at a time */

/** Most bytes term_telnet_open() writes. */
#define TERM_TELNET_OPEN_MAX 15u

/** Most answer bytes one received byte can cause. */
#define TERM_TELNET_REPLY_MAX 3u

/** Where the receive side stands. */
typedef enum TermTelnetRx
{
    TERM_TN_RX_DATA = 0,
    TERM_TN_RX_IAC,  /**< IAC seen */
    TERM_TN_RX_VERB, /**< WILL/WONT/DO/DONT seen: the option byte follows */
    TERM_TN_RX_SB,   /**< inside a subnegotiation */
    TERM_TN_RX_SB_IAC
} TermTelnetRx;

/** Codec state for one connection. */
typedef struct TermTelnet
{
    bool binary; /**< offer and accept BINARY (8-bit clean) */
    TermTelnetRx rx;
    uint8_t verb;       /**< the WILL/WONT/DO/DONT whose option byte comes next */
    uint8_t us_on[32];  /**< one bit per option WE do (agreed) */
    uint8_t him_on[32]; /**< one bit per option the CLIENT does (agreed) */
    bool after_cr;      /**< the last data byte was CR (for the CR NUL rule) */
} TermTelnet;

/**
 * @brief Start a codec.
 * @param t      Codec.
 * @param binary Also offer and accept BINARY (the host tool does; XMSGGW does not).
 */
void term_telnet_init(TermTelnet *t, bool binary);

/**
 * @brief The server's opening negotiation, to send first.
 * @param t   Codec.
 * @param out Buffer.
 * @param cap Its size (TERM_TELNET_OPEN_MAX is enough).
 * @return The byte count, or -1 for a NULL argument or a buffer that is too small.
 */
int term_telnet_open(TermTelnet *t, uint8_t *out, size_t cap);

/**
 * @brief Feed one byte read from the socket.
 * @param t         Codec.
 * @param b         The byte.
 * @param reply     Receives the answer bytes to send back (an option answer), if any.
 * @param reply_len Receives their count (0..TERM_TELNET_REPLY_MAX).
 * @return The data byte (0..255) the user typed; -1 when the byte was part of a telnet command
 *         or was dropped (the NUL of CR NUL); -2 for a NULL argument.
 */
int term_telnet_rx(TermTelnet *t, uint8_t b, uint8_t reply[TERM_TELNET_REPLY_MAX],
                   size_t *reply_len);

/**
 * @brief Make bytes for the user safe to send: 0xFF becomes IAC IAC.
 * @param in    Bytes.
 * @param n     Their count.
 * @param out   Buffer.
 * @param cap   Its size.
 * @param taken Receives how many bytes of `in` went into the output (all, unless `out` is full;
 *              a 0xFF is never split).
 * @return The output length, or -1 for a NULL argument.
 */
int term_telnet_escape(const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *taken);

/**
 * @brief Whether an option is agreed.
 * @param t      Codec.
 * @param opt    The option.
 * @param client true: the client does it; false: we do it.
 * @return true when agreed.
 */
bool term_telnet_is_on(const TermTelnet *t, uint8_t opt, bool client);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_TERM_TELNET_H */
