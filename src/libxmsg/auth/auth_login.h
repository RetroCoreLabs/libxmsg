/*
 * auth_login.h - the login dialogue of a command line: name, password, a few tries.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Fed whole lines (from a line editor) and a clock; says which prompt to show, whether to echo,
 * and when the user is in or has used up the tries. Never says whether the name or the password
 * was wrong. After a failure it waits `delay_ms` before asking again; lines typed during the
 * wait are thrown away. The password is checked through auth_user_verify() and never kept.
 *
 *   NAME --line--> PASSWORD --right--> DONE
 *                     |--wrong, tries left--> WAIT --delay--> NAME
 *                     |--wrong, last try----> REFUSED
 * An empty name line asks again for the name and is not a try.
 */
#ifndef XMSG_AUTH_LOGIN_H
#define XMSG_AUTH_LOGIN_H

#include <stdbool.h>
#include <stdint.h>

#include "libxmsg/auth/auth_user.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Default number of tries before the connection is refused. */
#define AUTH_LOGIN_TRIES 3u
/** Default wait after a failed try, in milliseconds. */
#define AUTH_LOGIN_DELAY_MS 1000u

/** Where the dialogue stands. */
typedef enum AuthLoginState
{
    AUTH_LOGIN_NAME = 0, /**< waiting for the name */
    AUTH_LOGIN_PASSWORD, /**< waiting for the password (do not echo) */
    AUTH_LOGIN_WAIT,     /**< a try failed; waiting before asking again */
    AUTH_LOGIN_DONE,     /**< logged in */
    AUTH_LOGIN_REFUSED   /**< tries used up: close the connection */
} AuthLoginState;

/** The dialogue. */
typedef struct AuthLogin
{
    AuthUserStore store;
    AuthLoginState state;
    uint32_t max_tries;
    uint32_t delay_ms;
    uint32_t failures;
    uint32_t wait_until_ms;
    char name[AUTH_NAME_MAX + 1u]; /**< the name typed; after DONE the logged-in user */
} AuthLogin;

/**
 * @brief Start a dialogue.
 * @param l         Dialogue.
 * @param store     Where users are looked up (copied).
 * @param max_tries Tries before REFUSED, at least 1.
 * @param delay_ms  Wait after a failure.
 * @return 0, or -1 for a bad argument.
 */
int auth_login_init(AuthLogin *l, const AuthUserStore *store, uint32_t max_tries,
                    uint32_t delay_ms);

/**
 * @brief Give the dialogue one typed line.
 * @param l      Dialogue.
 * @param line   The line (a name or a password); the caller wipes it afterwards.
 * @param now_ms Current time.
 * @return The state after the line.
 */
AuthLoginState auth_login_line(AuthLogin *l, const char *line, uint32_t now_ms);

/**
 * @brief Let time pass: ends a WAIT once the delay is over.
 * @param l      Dialogue.
 * @param now_ms Current time.
 * @return true when the state changed (a new prompt is due).
 */
bool auth_login_tick(AuthLogin *l, uint32_t now_ms);

/**
 * @brief The prompt to show in the current state.
 * @param l Dialogue.
 * @return "login: ", "password: ", or NULL when no prompt is due (WAIT, DONE, REFUSED).
 */
const char *auth_login_prompt(const AuthLogin *l);

/**
 * @brief Whether typed characters should be echoed now (not while a password is typed).
 * @param l Dialogue.
 * @return true to echo.
 */
bool auth_login_echo(const AuthLogin *l);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_AUTH_LOGIN_H */
