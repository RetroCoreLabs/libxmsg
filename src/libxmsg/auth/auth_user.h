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
 *     name:pbkdf2-sha256:<iterations>:<salt hex, 32 digits>:<hash hex, 64 digits>
 * A line starting with '#' and an empty line are comments.
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
/** Room for one formatted line, with its zero (name + scheme + count + hex + separators). */
#define AUTH_LINE_MAX (AUTH_NAME_MAX + 14u + 11u + (2u * AUTH_SALT_SIZE) + (2u * AUTH_HASH_SIZE) + 8u)

/** One user. */
typedef struct AuthUser
{
    char name[AUTH_NAME_MAX + 1u];
    uint32_t iterations;
    uint8_t salt[AUTH_SALT_SIZE];
    uint8_t hash[AUTH_HASH_SIZE];
} AuthUser;

/** Where the users live. `lookup` returns 0 and fills `out` when the name is known, -1 when
 *  not. Names are matched exactly (the store decides; the host tool matches case-sensitively). */
typedef struct AuthUserStore
{
    void *ctx;
    int (*lookup)(void *ctx, const char *name, AuthUser *out);
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
