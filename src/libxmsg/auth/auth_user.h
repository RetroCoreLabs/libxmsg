/*
 * auth_user.h - user records with PBKDF2-HMAC-SHA256 password hashes, and the login check.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * A user record holds the name, the PBKDF2 iteration count, a random 16-byte salt and the
 * 32-byte result. The password itself is never stored. The library never touches files: the
 * application looks users up through AuthUserStore and supplies random bytes through
 * AuthRandom (BCryptGenRandom on Windows, getrandom or /dev/urandom on POSIX, the hardware
 * generator on a microcontroller).
 *
 * One text line per user, the form the host tool keeps in its users file:
 *     name:pbkdf2-sha256:<iterations>:<salt hex, 32 digits>:<hash hex, 64 digits>:<flags>
 * where <flags> is "admin" or "user", optionally followed by ",change" (for example
 * "admin,change"): the level, and whether the user must set a new password at the next login
 * before anything else works. Any other flag word makes the line bad. A line with only the first
 * five fields (the form before the flags existed) is read as level user, no change needed; the
 * formatter always writes all six. A line starting with '#' and an empty line are comments.
 *
 * Two levels: an admin may add and delete users, reset any password and change settings; a user
 * may use ND sessions and change their own password. The library only carries the level; the
 * application decides what each level may do.
 */
#ifndef XMSG_AUTH_USER_H
#define XMSG_AUTH_USER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libxmsg/auth/auth_hash.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Longest user name, without the terminating zero. */
#define AUTH_NAME_MAX 32u
/** Salt size. */
#define AUTH_SALT_SIZE 16u
/** Hash size. */
#define AUTH_HASH_SIZE 32u
/** Default PBKDF2 iteration count for new users. */
#define AUTH_DEFAULT_ITERATIONS 100000u
/** The scheme word in a users-file line. */
#define AUTH_SCHEME "pbkdf2-sha256"
/** Room for one formatted line, with its zero (name + scheme + count + hex + flags + separators). */
#define AUTH_LINE_MAX \
    (AUTH_NAME_MAX + 14u + 11u + (2u * AUTH_SALT_SIZE) + (2u * AUTH_HASH_SIZE) + 8u + 14u)
/** The default admin made by auth_user_make_default_admin(): name and password. */
#define AUTH_DEFAULT_ADMIN "admin"

/** What a user may do. */
typedef enum AuthLevel
{
    AUTH_LEVEL_USER = 0, /**< ND sessions, and change their own password */
    AUTH_LEVEL_ADMIN     /**< also add and delete users, reset passwords, change settings */
} AuthLevel;

/** One user. */
typedef struct AuthUser
{
    char name[AUTH_NAME_MAX + 1u];
    uint32_t iterations;
    uint8_t salt[AUTH_SALT_SIZE];
    uint8_t hash[AUTH_HASH_SIZE];
    AuthLevel level;
    bool must_change; /**< must set a new password at the next login before anything else */
} AuthUser;

/** Where the users live. `lookup` returns 0 and fills `out` when the name is known, -1 when
 *  not. Names are matched exactly (the store decides; the host tool matches case-sensitively).
 *  `save` stores a changed record (a new password), replacing the one of the same name, and
 *  returns 0 when it is kept; it may be NULL, and then no password can be changed through the
 *  login dialogue (a user who must change one cannot log in). */
typedef struct AuthUserStore
{
    void *ctx;
    int (*lookup)(void *ctx, const char *name, AuthUser *out);
    int (*save)(void *ctx, const AuthUser *u);
} AuthUserStore;

/** Random bytes for salts. `fill` returns 0 when all `len` bytes were written. */
typedef struct AuthRandom
{
    void *ctx;
    int (*fill)(void *ctx, uint8_t *buf, size_t len);
} AuthRandom;

/**
 * @brief Whether a name is allowed: 1..32 characters, printable ASCII, no ':' and no blank.
 * @param name The name.
 * @return true when allowed.
 */
bool auth_name_is_valid(const char *name);

/**
 * @brief Make a user record for a new password: a fresh salt and the PBKDF2 result.
 * @param out        Receives the record.
 * @param name       User name (see auth_name_is_valid).
 * @param password   The password, zero-terminated; must not be empty.
 * @param iterations PBKDF2 count, at least 1 (AUTH_DEFAULT_ITERATIONS for real use).
 * @param rng        Random source.
 * @return 0; -1 for a bad argument or an empty password; -2 when the random source failed.
 *         The record has level user and must_change false.
 */
int auth_user_make(AuthUser *out, const char *name, const char *password, uint32_t iterations,
                   const AuthRandom *rng);

/**
 * @brief Check a password against a record (constant-time compare of the result).
 * @param u        The record.
 * @param password The password typed.
 * @return true when it matches.
 */
bool auth_user_check(const AuthUser *u, const char *password);

/**
 * @brief Check a name and password through a store. An unknown name still costs one full
 *        PBKDF2 run, so the time taken does not tell whether the name exists.
 * @param store    The store.
 * @param name     The name typed.
 * @param password The password typed.
 * @return true when the user exists and the password matches.
 */
bool auth_user_verify(const AuthUserStore *store, const char *name, const char *password);

/**
 * @brief As auth_user_verify(), and on success also hand back the record (level, flags). An
 *        unknown name costs the same time as a known one with a wrong password.
 * @param store    The store.
 * @param name     The name typed.
 * @param password The password typed.
 * @param out      Receives the record on success; zeroed on failure.
 * @return true when the user exists and the password matches.
 */
bool auth_user_verify_get(const AuthUserStore *store, const char *name, const char *password,
                          AuthUser *out);

/** Results of auth_user_change_password() and auth_user_reset_password(). */
#define AUTH_CHANGE_OK        0  /**< changed */
#define AUTH_CHANGE_BAD_ARG   -1 /**< bad argument */
#define AUTH_CHANGE_EMPTY     -2 /**< the new password is empty */
#define AUTH_CHANGE_RANDOM    -3 /**< the random source failed */
#define AUTH_CHANGE_WRONG_OLD -4 /**< the old password does not match */
#define AUTH_CHANGE_SAME      -5 /**< the new password is the old one */

/**
 * @brief A user changes their own password: the old one must match, the new one must not be
 *        empty and must differ from the old. A fresh salt is drawn; the iteration count, name and
 *        level stay; must_change is cleared. On any failure the record is left as it was.
 * @param u      The record, changed in place.
 * @param old_pw The current password.
 * @param new_pw The new password.
 * @param rng    Random source for the salt.
 * @return AUTH_CHANGE_OK or one of the negative AUTH_CHANGE_ codes.
 */
int auth_user_change_password(AuthUser *u, const char *old_pw, const char *new_pw,
                              const AuthRandom *rng);

/**
 * @brief An admin sets a new password for a user: fresh salt, the iteration count, name and
 *        level stay, and must_change is set so the user picks their own at the next login. On
 *        any failure the record is left as it was.
 * @param u      The record, changed in place.
 * @param new_pw The new password, not empty.
 * @param rng    Random source for the salt.
 * @return AUTH_CHANGE_OK, AUTH_CHANGE_BAD_ARG, AUTH_CHANGE_EMPTY or AUTH_CHANGE_RANDOM.
 */
int auth_user_reset_password(AuthUser *u, const char *new_pw, const AuthRandom *rng);

/**
 * @brief The record a new installation starts with: name "admin", password "admin", level
 *        admin, must_change true, so the first login has to set a real password.
 * @param out        Receives the record.
 * @param iterations PBKDF2 count, at least 1.
 * @param rng        Random source.
 * @return 0; -1 for a bad argument; -2 when the random source failed.
 */
int auth_user_make_default_admin(AuthUser *out, uint32_t iterations, const AuthRandom *rng);

/**
 * @brief Whether `name` is the only admin in a list, so deleting it, or making it a plain user,
 *        would leave nobody who can manage users.
 * @param users The records.
 * @param n     Their count.
 * @param name  The name to look at.
 * @return true when `name` is in the list, is an admin, and no other record is.
 */
bool auth_users_is_last_admin(const AuthUser *users, size_t n, const char *name);

/**
 * @brief Read one users-file line.
 * @param line The line, with or without a trailing CR/LF.
 * @param out  Receives the record.
 * @return 0 for a user; 1 for a comment or empty line (out untouched); -1 for a bad line.
 */
int auth_user_parse_line(const char *line, AuthUser *out);

/**
 * @brief Write one users-file line, without a newline.
 * @param u   The record.
 * @param buf Buffer.
 * @param cap Its size (AUTH_LINE_MAX is enough).
 * @return The length written, or -1 when it does not fit or the record is bad.
 */
int auth_user_format_line(const AuthUser *u, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_AUTH_USER_H */
