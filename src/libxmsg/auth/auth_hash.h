/*
 * auth_hash.h - SHA-256, HMAC-SHA256 and PBKDF2-HMAC-SHA256, with no outside dependency.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Written from the standards, for password checks on any target (PC, ESP32, Pico):
 *   SHA-256       FIPS 180-4 (same algorithm as FIPS 180-2)
 *   HMAC          RFC 2104 / FIPS 198-1
 *   PBKDF2        RFC 8018 section 5.2 (formerly RFC 2898)
 * Tested against the FIPS 180-2 examples, RFC 4231 cases 1-2 and PBKDF2 values computed with
 * Python hashlib (extras/tests/test_auth.c).
 */
#ifndef XMSG_AUTH_HASH_H
#define XMSG_AUTH_HASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** SHA-256 output size in bytes. */
#define AUTH_SHA256_SIZE 32u
/** SHA-256 block size in bytes. */
#define AUTH_SHA256_BLOCK 64u

/** SHA-256 running state. */
typedef struct AuthSha256
{
    uint32_t h[8];
    uint64_t total; /**< bytes fed so far */
    uint8_t block[AUTH_SHA256_BLOCK];
    size_t fill; /**< bytes in block */
} AuthSha256;

/**
 * @brief Start a SHA-256.
 * @param s State.
 */
void auth_sha256_init(AuthSha256 *s);

/**
 * @brief Feed bytes.
 * @param s    State.
 * @param data Bytes; may be NULL when len is 0.
 * @param len  Their count.
 */
void auth_sha256_update(AuthSha256 *s, const uint8_t *data, size_t len);

/**
 * @brief Finish and give the digest. The state is wiped.
 * @param s   State.
 * @param out Receives 32 bytes.
 */
void auth_sha256_final(AuthSha256 *s, uint8_t out[AUTH_SHA256_SIZE]);

/**
 * @brief SHA-256 of one buffer.
 * @param data Bytes; may be NULL when len is 0.
 * @param len  Their count.
 * @param out  Receives 32 bytes.
 */
void auth_sha256(const uint8_t *data, size_t len, uint8_t out[AUTH_SHA256_SIZE]);

/**
 * @brief HMAC-SHA256.
 * @param key     Key; may be NULL when key_len is 0.
 * @param key_len Its length (any; longer than 64 is hashed first, as RFC 2104 says).
 * @param msg     Message; may be NULL when msg_len is 0.
 * @param msg_len Its length.
 * @param out     Receives 32 bytes.
 */
void auth_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                      uint8_t out[AUTH_SHA256_SIZE]);

/**
 * @brief PBKDF2 with HMAC-SHA256 (RFC 8018 section 5.2).
 * @param pw         Password bytes; may be NULL when pw_len is 0.
 * @param pw_len     Their count.
 * @param salt       Salt bytes.
 * @param salt_len   Their count (at most 64).
 * @param iterations The count c, at least 1.
 * @param out        Receives the derived key.
 * @param out_len    Its length, 1..64 here (two blocks are enough for every use in this
 *                   library).
 * @return 0, or -1 for a bad argument.
 */
int auth_pbkdf2_sha256(const uint8_t *pw, size_t pw_len, const uint8_t *salt, size_t salt_len,
                       uint32_t iterations, uint8_t *out, size_t out_len);

/**
 * @brief Compare two buffers in time that does not depend on where they differ.
 * @param a   First.
 * @param b   Second.
 * @param len Bytes to compare.
 * @return true when equal.
 */
bool auth_equal_ct(const uint8_t *a, const uint8_t *b, size_t len);

/**
 * @brief Overwrite memory with zeros in a way the compiler does not remove.
 * @param p   Memory.
 * @param len Bytes.
 */
void auth_wipe(void *p, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* XMSG_AUTH_HASH_H */
