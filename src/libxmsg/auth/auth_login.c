/*
 * auth_login.c - the login dialogue of a command line.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/auth/auth_login.h"

#include <string.h>

int auth_login_init(AuthLogin *l, const AuthUserStore *store, const AuthRandom *rng,
                    uint32_t max_tries, uint32_t delay_ms)
{
    if (l == NULL || store == NULL || store->lookup == NULL || max_tries == 0u)
    {
        return -1;
    }
    memset(l, 0, sizeof *l);
    l->store = *store;
    if (rng != NULL)
    {
        l->rng = *rng;
    }
    l->max_tries = max_tries;
    l->delay_ms = delay_ms;
    l->state = AUTH_LOGIN_NAME;
    return 0;
}

static bool can_change(const AuthLogin *l)
{
    return l->store.save != NULL && l->rng.fill != NULL;
}

/* Leave a change: wipe the half-made record. A forced change that cannot finish refuses the
 * login; a voluntary one goes back to DONE with the old record. */
static void change_end(AuthLogin *l, AuthLoginNote note)
{
    auth_wipe(&l->pending, sizeof l->pending);
    l->note = note;
    if (l->user.must_change)
    {
        auth_wipe(&l->user, sizeof l->user);
        memset(l->name, 0, sizeof l->name);
        l->state = AUTH_LOGIN_REFUSED;
        return;
    }
    l->state = AUTH_LOGIN_DONE;
}

static void password_line(AuthLogin *l, const char *line, uint32_t now_ms)
{
    if (auth_user_verify_get(&l->store, l->name, line, &l->user))
    {
        if (!l->user.must_change)
        {
            l->state = AUTH_LOGIN_DONE;
            return;
        }
        if (!can_change(l))
        {
            change_end(l, AUTH_NOTE_NO_SAVE);
            return;
        }
        l->state = AUTH_LOGIN_NEW_PASSWORD;
        return;
    }
    memset(l->name, 0, sizeof l->name);
    l->failures++;
    if (l->failures >= l->max_tries)
    {
        l->state = AUTH_LOGIN_REFUSED;
        return;
    }
    l->state = AUTH_LOGIN_WAIT;
    l->wait_until_ms = now_ms + l->delay_ms;
}

/* First entry of the new password: hashed at once, never kept as text. */
static void new_line(AuthLogin *l, const char *line)
{
    int rc;
    if (line[0] == '\0')
    {
        l->note = AUTH_NOTE_EMPTY;
        return;
    }
    if (auth_user_check(&l->user, line))
    {
        l->note = AUTH_NOTE_SAME;
        return;
    }
    l->pending = l->user;
    rc = auth_user_reset_password(&l->pending, line, &l->rng);
    if (rc != AUTH_CHANGE_OK)
    {
        change_end(l, AUTH_NOTE_RANDOM);
        return;
    }
    l->pending.must_change = false;
    l->state = AUTH_LOGIN_NEW_AGAIN;
}

/* Second entry: must give the hash of the first. */
static void again_line(AuthLogin *l, const char *line)
{
    if (!auth_user_check(&l->pending, line))
    {
        auth_wipe(&l->pending, sizeof l->pending);
        l->note = AUTH_NOTE_MISMATCH;
        l->state = AUTH_LOGIN_NEW_PASSWORD;
        return;
    }
    if (l->store.save(l->store.ctx, &l->pending) != 0)
    {
        change_end(l, AUTH_NOTE_SAVE_FAILED);
        return;
    }
    l->user = l->pending;
    auth_wipe(&l->pending, sizeof l->pending);
    l->note = AUTH_NOTE_CHANGED;
    l->state = AUTH_LOGIN_DONE;
}

AuthLoginState auth_login_line(AuthLogin *l, const char *line, uint32_t now_ms)
{
    size_t n;

    if (l == NULL || line == NULL)
    {
        return AUTH_LOGIN_REFUSED;
    }
    l->note = AUTH_NOTE_NONE;
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
        password_line(l, line, now_ms);
        break;
    case AUTH_LOGIN_OLD_PASSWORD:
        if (auth_user_check(&l->user, line))
        {
            l->state = AUTH_LOGIN_NEW_PASSWORD;
        }
        else
        {
            l->note = AUTH_NOTE_WRONG_OLD;
            l->state = AUTH_LOGIN_DONE;
        }
        break;
    case AUTH_LOGIN_NEW_PASSWORD:
        new_line(l, line);
        break;
    case AUTH_LOGIN_NEW_AGAIN:
        again_line(l, line);
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
    switch (l->state)
    {
    case AUTH_LOGIN_NAME:
        return "login: ";
    case AUTH_LOGIN_PASSWORD:
        return "password: ";
    case AUTH_LOGIN_OLD_PASSWORD:
        return "old password: ";
    case AUTH_LOGIN_NEW_PASSWORD:
        return "new password: ";
    case AUTH_LOGIN_NEW_AGAIN:
        return "again: ";
    default:
        return NULL;
    }
}

bool auth_login_echo(const AuthLogin *l)
{
    return l != NULL && l->state != AUTH_LOGIN_PASSWORD && l->state != AUTH_LOGIN_OLD_PASSWORD &&
           l->state != AUTH_LOGIN_NEW_PASSWORD && l->state != AUTH_LOGIN_NEW_AGAIN;
}

AuthLevel auth_login_level(const AuthLogin *l)
{
    bool logged_in;
    if (l == NULL)
    {
        return AUTH_LEVEL_USER;
    }
    /* Logged in: DONE, or a voluntary change (a forced change is not logged in yet). */
    logged_in = l->state == AUTH_LOGIN_DONE || l->state == AUTH_LOGIN_OLD_PASSWORD ||
                ((l->state == AUTH_LOGIN_NEW_PASSWORD || l->state == AUTH_LOGIN_NEW_AGAIN) &&
                 !l->user.must_change);
    return logged_in ? l->user.level : AUTH_LEVEL_USER;
}

bool auth_login_must_change(const AuthLogin *l)
{
    return l != NULL && l->user.must_change &&
           (l->state == AUTH_LOGIN_NEW_PASSWORD || l->state == AUTH_LOGIN_NEW_AGAIN);
}

const AuthUser *auth_login_user(const AuthLogin *l)
{
    return (l != NULL && l->state == AUTH_LOGIN_DONE) ? &l->user : NULL;
}

int auth_login_change_begin(AuthLogin *l)
{
    if (l == NULL || l->state != AUTH_LOGIN_DONE)
    {
        return -1;
    }
    if (!can_change(l))
    {
        l->note = AUTH_NOTE_NO_SAVE;
        return -1;
    }
    l->note = AUTH_NOTE_NONE;
    l->state = AUTH_LOGIN_OLD_PASSWORD;
    return 0;
}

AuthLoginNote auth_login_take_note(AuthLogin *l)
{
    AuthLoginNote n;
    if (l == NULL)
    {
        return AUTH_NOTE_NONE;
    }
    n = l->note;
    l->note = AUTH_NOTE_NONE;
    return n;
}

const char *auth_login_note_text(AuthLoginNote note)
{
    switch (note)
    {
    case AUTH_NOTE_EMPTY:
        return "an empty password is not allowed";
    case AUTH_NOTE_SAME:
        return "the new password must differ from the old one";
    case AUTH_NOTE_MISMATCH:
        return "the two passwords differ";
    case AUTH_NOTE_WRONG_OLD:
        return "the old password is wrong; nothing changed";
    case AUTH_NOTE_NO_SAVE:
        return "a new password must be set, but it cannot be stored here";
    case AUTH_NOTE_SAVE_FAILED:
        return "the new password could not be stored";
    case AUTH_NOTE_RANDOM:
        return "no random bytes for the new password";
    case AUTH_NOTE_CHANGED:
        return "password changed";
    default:
        return "";
    }
}
