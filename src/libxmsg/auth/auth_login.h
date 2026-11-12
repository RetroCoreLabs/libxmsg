/*
 * auth_login.h - the login dialogue of a command line: name, password, a few tries, and a new
 * password when the user must set one.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Fed whole lines (from a line editor) and a clock; says which prompt to show, whether to echo,
 * and when the user is in or has used up the tries. Never says whether the name or the password
 * was wrong. After a failure it waits `delay_ms` before asking again; lines typed during the
 * wait are thrown away. The password is checked through auth_user_verify_get() and never kept.
 *
 *   NAME --line--> PASSWORD --right--> DONE
 *                     |--right, must_change--> NEW_PASSWORD --> NEW_AGAIN --same--> DONE
 *                     |--wrong, tries left--> WAIT --delay--> NAME
 *                     |--wrong, last try----> REFUSED
 * An empty name line asks again for the name and is not a try.
 *
 * Changing the password (NEW_PASSWORD, NEW_AGAIN): the new password is asked twice. An empty
 * one, one equal to the old, or a second entry that differs from the first is refused with a
 * note (auth_login_take_note) and the new password is asked for again. The first entry is never
 * kept as text: it is hashed at once with a fresh salt, and the second entry is checked against
 * that hash. When both agree the new record (must_change cleared) goes to the store's `save`
 * and the dialogue reaches DONE; auth_login_user() then returns it. A store without `save`, or
 * no random source, makes the change impossible: a user who must change their password is then
 * REFUSED with the note AUTH_NOTE_NO_SAVE. A save that fails also ends in REFUSED.
 *
 * After DONE, auth_login_change_begin() starts a voluntary change of one's own password
 * (OLD_PASSWORD, then the same two steps). A wrong old password, or a failed save, ends it at
 * DONE again with a note and the record unchanged.
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
    AUTH_LOGIN_NAME = 0,     /**< waiting for the name */
    AUTH_LOGIN_PASSWORD,     /**< waiting for the password (do not echo) */
    AUTH_LOGIN_WAIT,         /**< a try failed; waiting before asking again */
    AUTH_LOGIN_DONE,         /**< logged in */
    AUTH_LOGIN_REFUSED,      /**< tries used up, or a needed change impossible: close */
    AUTH_LOGIN_OLD_PASSWORD, /**< voluntary change: waiting for the current password */
    AUTH_LOGIN_NEW_PASSWORD, /**< waiting for the new password */
    AUTH_LOGIN_NEW_AGAIN     /**< waiting for the new password a second time */
} AuthLoginState;

/** Why a step of a password change was refused; the application turns it into text. */
typedef enum AuthLoginNote
{
    AUTH_NOTE_NONE = 0,
    AUTH_NOTE_EMPTY,       /**< the new password was empty */
    AUTH_NOTE_SAME,        /**< the new password is the old one */
    AUTH_NOTE_MISMATCH,    /**< the second entry differs from the first */
    AUTH_NOTE_WRONG_OLD,   /**< the current password was wrong (voluntary change) */
    AUTH_NOTE_NO_SAVE,     /**< the store cannot save, or no random source: no change possible */
    AUTH_NOTE_SAVE_FAILED, /**< the store did not keep the new record */
    AUTH_NOTE_RANDOM,      /**< the random source failed */
    AUTH_NOTE_CHANGED      /**< the new password is stored */
} AuthLoginNote;

/** The dialogue. */
typedef struct AuthLogin
{
    AuthUserStore store;
    AuthRandom rng;
    AuthLoginState state;
    AuthLoginNote note;
    uint32_t max_tries;
    uint32_t delay_ms;
    uint32_t failures;
    uint32_t wait_until_ms;
    char name[AUTH_NAME_MAX + 1u]; /**< the name typed; after DONE the logged-in user */
    AuthUser user;                 /**< the logged-in record (hashes only, no password) */
    AuthUser pending;              /**< the new record between the two entries */
} AuthLogin;

/**
 * @brief Start a dialogue.
 * @param l         Dialogue.
 * @param store     Where users are looked up and changed records saved (copied).
 * @param rng       Random source for new salts (copied); NULL when no change is possible.
 * @param max_tries Tries before REFUSED, at least 1.
 * @param delay_ms  Wait after a failure.
 * @return 0, or -1 for a bad argument.
 */
int auth_login_init(AuthLogin *l, const AuthUserStore *store, const AuthRandom *rng,
                    uint32_t max_tries, uint32_t delay_ms);

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
 * @return "login: ", "password: ", "old password: ", "new password: ", "again: ", or NULL when
 *         no prompt is due (WAIT, DONE, REFUSED).
 */
const char *auth_login_prompt(const AuthLogin *l);

/**
 * @brief Whether typed characters should be echoed now (not while a password is typed).
 * @param l Dialogue.
 * @return true to echo.
 */
bool auth_login_echo(const AuthLogin *l);

/**
 * @brief The level of the user who logged in.
 * @param l Dialogue.
 * @return The level once DONE (or in a voluntary change); AUTH_LEVEL_USER otherwise.
 */
AuthLevel auth_login_level(const AuthLogin *l);

/**
 * @brief Whether the user whose password was accepted must still set a new one.
 * @param l Dialogue.
 * @return true while the forced change is under way; false once DONE.
 */
bool auth_login_must_change(const AuthLogin *l);

/**
 * @brief The record of the logged-in user, after any change (the new hash).
 * @param l Dialogue.
 * @return The record when DONE, else NULL.
 */
const AuthUser *auth_login_user(const AuthLogin *l);

/**
 * @brief Start a voluntary change of the logged-in user's own password.
 * @param l Dialogue, in DONE.
 * @return 0 (state OLD_PASSWORD); -1 when not DONE, or when the store cannot save or there is
 *         no random source (note AUTH_NOTE_NO_SAVE, state stays DONE).
 */
int auth_login_change_begin(AuthLogin *l);

/**
 * @brief Read and clear the note left by the last line.
 * @param l Dialogue.
 * @return The note, AUTH_NOTE_NONE when there is none.
 */
AuthLoginNote auth_login_take_note(AuthLogin *l);

/**
 * @brief A short English text for a note, for the application to show.
 * @param note The note.
 * @return The text, never NULL ("" for AUTH_NOTE_NONE).
 */
const char *auth_login_note_text(AuthLoginNote note);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_AUTH_LOGIN_H */
