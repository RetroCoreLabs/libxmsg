/*
 * auth_login.c - the login dialogue of a command line.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/auth/auth_login.h"

#include <string.h>

int auth_login_init(AuthLogin *l, const AuthUserStore *store, uint32_t max_tries,
                    uint32_t delay_ms)
{
    if (l == NULL || store == NULL || store->lookup == NULL || max_tries == 0u)
    {
        return -1;
    }
    memset(l, 0, sizeof *l);
    l->store = *store;
    l->max_tries = max_tries;
    l->delay_ms = delay_ms;
    l->state = AUTH_LOGIN_NAME;
    return 0;
}

AuthLoginState auth_login_line(AuthLogin *l, const char *line, uint32_t now_ms)
{
    size_t n;

    if (l == NULL || line == NULL)
    {
        return AUTH_LOGIN_REFUSED;
    }
    switch (l->state)
    {
    case AUTH_LOGIN_NAME:
        n = strlen(line);
        if (n == 0)
        {
            break; /* ask again; not a try */
        }
        /* A name too long to be valid is kept cut short; it can then never match, and the
         * password is still asked for, so the dialogue does not tell that it was wrong. */
        if (n > AUTH_NAME_MAX)
        {
            n = AUTH_NAME_MAX;
        }
        memset(l->name, 0, sizeof l->name);
        memcpy(l->name, line, n);
        l->state = AUTH_LOGIN_PASSWORD;
        break;
    case AUTH_LOGIN_PASSWORD:
        if (auth_user_verify(&l->store, l->name, line))
        {
            l->state = AUTH_LOGIN_DONE;
            break;
        }
        memset(l->name, 0, sizeof l->name);
        l->failures++;
        if (l->failures >= l->max_tries)
        {
            l->state = AUTH_LOGIN_REFUSED;
            break;
        }
        l->state = AUTH_LOGIN_WAIT;
        l->wait_until_ms = now_ms + l->delay_ms;
        break;
    default:
        break; /* WAIT: thrown away; DONE and REFUSED: nothing more to do */
    }
    return l->state;
}

bool auth_login_tick(AuthLogin *l, uint32_t now_ms)
{
    if (l == NULL || l->state != AUTH_LOGIN_WAIT)
    {
        return false;
    }
    if ((int32_t)(now_ms - l->wait_until_ms) < 0)
    {
        return false;
    }
    l->state = AUTH_LOGIN_NAME;
    return true;
}

const char *auth_login_prompt(const AuthLogin *l)
{
    if (l == NULL)
    {
        return NULL;
    }
    if (l->state == AUTH_LOGIN_NAME)
    {
        return "login: ";
    }
    if (l->state == AUTH_LOGIN_PASSWORD)
    {
        return "password: ";
    }
    return NULL;
}

bool auth_login_echo(const AuthLogin *l)
{
    return l != NULL && l->state != AUTH_LOGIN_PASSWORD;
}
