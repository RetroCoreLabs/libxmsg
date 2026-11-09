/*
 * auth_user.c - user records with PBKDF2-HMAC-SHA256 password hashes, and the login check.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/auth/auth_user.h"

#include <string.h>

bool auth_name_is_valid(const char *name)
{
    size_t n;
    if (name == NULL)
    {
        return false;
    }
    n = strlen(name);
    if (n == 0 || n > AUTH_NAME_MAX)
    {
        return false;
    }
    for (size_t i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)name[i];
        if (c <= 0x20u || c > 0x7Eu || c == ':')
        {
            return false;
        }
    }
    return true;
}

static int derive(const char *password, const uint8_t *salt, uint32_t iterations,
                  uint8_t out[AUTH_HASH_SIZE])
{
    return auth_pbkdf2_sha256((const uint8_t *)password, strlen(password), salt, AUTH_SALT_SIZE,
                              iterations, out, AUTH_HASH_SIZE);
}

int auth_user_make(AuthUser *out, const char *name, const char *password, uint32_t iterations,
                   const AuthRandom *rng)
{
    if (out == NULL || !auth_name_is_valid(name) || password == NULL || password[0] == '\0' ||
        iterations == 0u || rng == NULL || rng->fill == NULL)
    {
        return -1;
    }
    memset(out, 0, sizeof *out);
    memcpy(out->name, name, strlen(name) + 1u);
    out->iterations = iterations;
    if (rng->fill(rng->ctx, out->salt, sizeof out->salt) != 0)
    {
        auth_wipe(out, sizeof *out);
        return -2;
    }
    if (derive(password, out->salt, iterations, out->hash) != 0)
    {
        auth_wipe(out, sizeof *out);
        return -1;
    }
    return 0;
}

bool auth_user_check(const AuthUser *u, const char *password)
{
    uint8_t got[AUTH_HASH_SIZE];
    bool ok;
    if (u == NULL || password == NULL || u->iterations == 0u)
    {
        return false;
    }
    if (derive(password, u->salt, u->iterations, got) != 0)
    {
        return false;
    }
    ok = auth_equal_ct(got, u->hash, sizeof got);
    auth_wipe(got, sizeof got);
    return ok;
}

bool auth_user_verify(const AuthUserStore *store, const char *name, const char *password)
{
    AuthUser u;
    bool known;
    bool ok;

    if (store == NULL || store->lookup == NULL || name == NULL || password == NULL)
    {
        return false;
    }
    memset(&u, 0, sizeof u);
    known = auth_name_is_valid(name) && store->lookup(store->ctx, name, &u) == 0;
    if (!known)
    {
        /* Do the same work against a record that cannot match, so an unknown name takes as long
         * as a known one with a wrong password. */
        memset(&u, 0, sizeof u);
        u.iterations = AUTH_DEFAULT_ITERATIONS;
        (void)auth_user_check(&u, password); /* result thrown away on purpose */
        auth_wipe(&u, sizeof u);
        return false;
    }
    ok = auth_user_check(&u, password);
    auth_wipe(&u, sizeof u);
    return ok;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

/* Read exactly 2*n hex digits from s into out. Returns 0 or -1. */
static int read_hex(const char *s, size_t digits, uint8_t *out, size_t n)
{
    if (digits != 2u * n)
    {
        return -1;
    }
    for (size_t i = 0; i < n; i++)
    {
        int hi = hex_value(s[2u * i]);
        int lo = hex_value(s[(2u * i) + 1u]);
        if (hi < 0 || lo < 0)
        {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

int auth_user_parse_line(const char *line, AuthUser *out)
{
    const char *f[5];
    size_t flen[5];
    size_t end;
    size_t nf = 0;
    size_t start = 0;
    uint32_t iters = 0;
    AuthUser u;

    if (line == NULL || out == NULL)
    {
        return -1;
    }
    end = strlen(line);
    while (end > 0 && (line[end - 1u] == '\n' || line[end - 1u] == '\r'))
    {
        end--;
    }
    if (end == 0 || line[0] == '#')
    {
        return 1;
    }
    for (size_t i = 0; i <= end; i++)
    {
        if (i == end || line[i] == ':')
        {
            if (nf == 5u)
            {
                return -1;
            }
            f[nf] = line + start;
            flen[nf] = i - start;
            nf++;
            start = i + 1u;
        }
    }
    if (nf != 5u || flen[0] == 0 || flen[0] > AUTH_NAME_MAX || flen[1] != strlen(AUTH_SCHEME) ||
        memcmp(f[1], AUTH_SCHEME, flen[1]) != 0 || flen[2] == 0 || flen[2] > 10u)
    {
        return -1;
    }
    memset(&u, 0, sizeof u);
    memcpy(u.name, f[0], flen[0]);
    if (!auth_name_is_valid(u.name))
    {
        return -1;
    }
    for (size_t i = 0; i < flen[2]; i++)
    {
        char c = f[2][i];
        if (c < '0' || c > '9' || iters > (UINT32_MAX - 9u) / 10u)
        {
            return -1;
        }
        iters = (iters * 10u) + (uint32_t)(c - '0');
    }
    if (iters == 0u || read_hex(f[3], flen[3], u.salt, sizeof u.salt) != 0 ||
        read_hex(f[4], flen[4], u.hash, sizeof u.hash) != 0)
    {
        return -1;
    }
    u.iterations = iters;
    *out = u;
    auth_wipe(&u, sizeof u);
    return 0;
}

static size_t put_hex(char *p, const uint8_t *b, size_t n)
{
    static const char k_digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++)
    {
        p[2u * i] = k_digits[b[i] >> 4u];
        p[(2u * i) + 1u] = k_digits[b[i] & 0x0Fu];
    }
    return 2u * n;
}

int auth_user_format_line(const AuthUser *u, char *buf, size_t cap)
{
    char count[11];
    size_t nc = 0;
    size_t n = 0;
    size_t name_len;
    size_t need;
    uint32_t v;

    if (u == NULL || buf == NULL || !auth_name_is_valid(u->name) || u->iterations == 0u)
    {
        return -1;
    }
    v = u->iterations;
    do
    {
        count[nc++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);
    name_len = strlen(u->name);
    need = name_len + 1u + strlen(AUTH_SCHEME) + 1u + nc + 1u + (2u * AUTH_SALT_SIZE) + 1u +
           (2u * AUTH_HASH_SIZE) + 1u;
    if (need > cap)
    {
        return -1;
    }
    memcpy(buf, u->name, name_len);
    n += name_len;
    buf[n++] = ':';
    memcpy(buf + n, AUTH_SCHEME, strlen(AUTH_SCHEME));
    n += strlen(AUTH_SCHEME);
    buf[n++] = ':';
    for (size_t i = 0; i < nc; i++)
    {
        buf[n++] = count[nc - 1u - i];
    }
    buf[n++] = ':';
    n += put_hex(buf + n, u->salt, sizeof u->salt);
    buf[n++] = ':';
    n += put_hex(buf + n, u->hash, sizeof u->hash);
    buf[n] = '\0';
    return (int)n;
}
