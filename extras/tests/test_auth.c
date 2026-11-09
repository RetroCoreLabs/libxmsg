/*
 * test_auth.c - SHA-256, HMAC-SHA256, PBKDF2, user records and the login dialogue.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Every expected value below was computed with Python's own hashlib / hmac (python -I) on
 * 2026-11-09, not written from memory:
 *   SHA-256 of "abc", "" and the 448-bit message: the FIPS 180-2 examples.
 *   HMAC-SHA256: RFC 4231 test cases 1, 2 and 6 (key longer than a block).
 *   PBKDF2-HMAC-SHA256: "password"/"salt" c=1 and c=4096 dkLen 32, the long pair c=4096
 *   dkLen 40 (two blocks), and "secret" with salt 00..0F c=10 (the user record test).
 */
#include "libxmsg/auth/auth_hash.h"
#include "libxmsg/auth/auth_login.h"
#include "libxmsg/auth/auth_user.h"
#include "test_util.h"

static void sha256_vectors(void)
{
    static const char k_448[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    uint8_t d[32];
    AuthSha256 s;

    auth_sha256((const uint8_t *)"abc", 3, d);
    CHECK_BYTES(d, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    auth_sha256(NULL, 0, d);
    CHECK_BYTES(d, 32, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    auth_sha256((const uint8_t *)k_448, strlen(k_448), d);
    CHECK_BYTES(d, 32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    /* The same message fed one byte at a time crosses the block edge inside update. */
    auth_sha256_init(&s);
    for (size_t i = 0; i < strlen(k_448); i++)
    {
        auth_sha256_update(&s, (const uint8_t *)k_448 + i, 1);
    }
    auth_sha256_final(&s, d);
    CHECK_BYTES(d, 32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

static void hmac_vectors(void)
{
    static const char k_msg6[] = "Test Using Larger Than Block-Size Key - Hash Key First";
    uint8_t key[131];
    uint8_t d[32];

    memset(key, 0x0B, 20);
    auth_hmac_sha256(key, 20, (const uint8_t *)"Hi There", 8, d);
    CHECK_BYTES(d, 32, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    auth_hmac_sha256((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?",
                     28, d);
    CHECK_BYTES(d, 32, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    memset(key, 0xAA, sizeof key);
    auth_hmac_sha256(key, sizeof key, (const uint8_t *)k_msg6, strlen(k_msg6), d);
    CHECK_BYTES(d, 32, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

static void pbkdf2_vectors(void)
{
    static const char k_pw[] = "passwordPASSWORDpassword";
    static const char k_salt[] = "saltSALTsaltSALTsaltSALTsaltSALTsalt";
    uint8_t d[40];

    CHECK_EQ(auth_pbkdf2_sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 1, d,
                                32),
             0);
    CHECK_BYTES(d, 32, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    CHECK_EQ(auth_pbkdf2_sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 4096,
                                d, 32),
             0);
    CHECK_BYTES(d, 32, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
    CHECK_EQ(auth_pbkdf2_sha256((const uint8_t *)k_pw, strlen(k_pw), (const uint8_t *)k_salt,
                                strlen(k_salt), 4096, d, 40),
             0);
    CHECK_BYTES(d, 40,
                "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1c635518c7dac47e9");
    CHECK_EQ(auth_pbkdf2_sha256(NULL, 0, (const uint8_t *)"s", 1, 0, d, 32), -1); /* c = 0 */
    CHECK_EQ(auth_pbkdf2_sha256(NULL, 0, (const uint8_t *)"s", 1, 1, d, 65), -1); /* too long */
}

static void equal_in_constant_time(void)
{
    uint8_t a[32];
    uint8_t b[32];
    memset(a, 0x5A, sizeof a);
    memcpy(b, a, sizeof b);
    CHECK(auth_equal_ct(a, b, sizeof a));
    b[0] ^= 0x01u;
    CHECK(!auth_equal_ct(a, b, sizeof a)); /* first byte */
    b[0] = a[0];
    b[31] ^= 0x80u;
    CHECK(!auth_equal_ct(a, b, sizeof a)); /* last byte, top bit */
    CHECK(auth_equal_ct(a, b, 31));
    CHECK(auth_equal_ct(a, b, 0));
    CHECK(!auth_equal_ct(NULL, b, 1));
    auth_wipe(a, sizeof a);
    CHECK_EQ(a[0] | a[31], 0);
}

/* ------------------------------------------------------------------ users */

static int fixed_salt(void *ctx, uint8_t *buf, size_t len)
{
    (void)ctx;
    for (size_t i = 0; i < len; i++)
    {
        buf[i] = (uint8_t)i;
    }
    return 0;
}

static int broken_random(void *ctx, uint8_t *buf, size_t len)
{
    (void)ctx;
    (void)buf;
    (void)len;
    return -1;
}

#define N_USERS 2
static AuthUser s_users[N_USERS];

static int lookup(void *ctx, const char *name, AuthUser *out)
{
    (void)ctx;
    for (int i = 0; i < N_USERS; i++)
    {
        if (strcmp(s_users[i].name, name) == 0)
        {
            *out = s_users[i];
            return 0;
        }
    }
    return -1;
}

static void user_records(void)
{
    const AuthRandom rng = {NULL, fixed_salt};
    const AuthRandom bad = {NULL, broken_random};
    AuthUser u;
    AuthUser back;
    char line[AUTH_LINE_MAX];

    CHECK_EQ(auth_user_make(&u, "ronny", "secret", 10, &rng), 0);
    CHECK_BYTES(u.salt, 16, "000102030405060708090A0B0C0D0E0F");
    CHECK_BYTES(u.hash, 32, "f8332ea7a39557af003df934f09c9a92e648b4fe4cd432aeea7b4d017c35ab3f");
    CHECK(auth_user_check(&u, "secret"));
    CHECK(!auth_user_check(&u, "Secret"));
    CHECK(!auth_user_check(&u, ""));

    int n = auth_user_format_line(&u, line, sizeof line);
    CHECK(strcmp(line, "ronny:pbkdf2-sha256:10:000102030405060708090a0b0c0d0e0f:"
                       "f8332ea7a39557af003df934f09c9a92e648b4fe4cd432aeea7b4d017c35ab3f") == 0);
    CHECK_EQ(n, (int)strlen(line));
    CHECK_EQ(auth_user_parse_line(line, &back), 0);
    CHECK(memcmp(&back, &u, sizeof u) == 0); /* round trip */
    CHECK_EQ(auth_user_format_line(&u, line, 20), -1);

    /* Upper-case hex and a CR LF end are read too. */
    CHECK_EQ(auth_user_parse_line("ronny:pbkdf2-sha256:10:000102030405060708090A0B0C0D0E0F:"
                                  "F8332EA7A39557AF003DF934F09C9A92E648B4FE4CD432AEEA7B4D017C35AB3F"
                                  "\r\n",
                                  &back),
             0);
    CHECK(auth_user_check(&back, "secret"));
    CHECK_EQ(auth_user_parse_line("# comment", &back), 1);
    CHECK_EQ(auth_user_parse_line("", &back), 1);
    CHECK_EQ(auth_user_parse_line("\n", &back), 1);
    /* Bad lines: wrong scheme, count 0, short salt, extra field, no name, bad hex. */
    CHECK_EQ(auth_user_parse_line("a:md5:10:000102030405060708090a0b0c0d0e0f:" /* scheme */
                                  "f8332ea7a39557af003df934f09c9a92e648b4fe4cd432aeea7b4d017c35ab3f",
                                  &back),
             -1);
    CHECK_EQ(auth_user_parse_line("a:pbkdf2-sha256:0:000102030405060708090a0b0c0d0e0f:"
                                  "f8332ea7a39557af003df934f09c9a92e648b4fe4cd432aeea7b4d017c35ab3f",
                                  &back),
             -1);
    CHECK_EQ(auth_user_parse_line("a:pbkdf2-sha256:10:0001:"
                                  "f8332ea7a39557af003df934f09c9a92e648b4fe4cd432aeea7b4d017c35ab3f",
                                  &back),
             -1);
    CHECK_EQ(auth_user_parse_line("a:pbkdf2-sha256:10:000102030405060708090a0b0c0d0e0f:"
                                  "f8332ea7a39557af003df934f09c9a92e648b4fe4cd432aeea7b4d017c35ab3f:x",
                                  &back),
             -1);
    CHECK_EQ(auth_user_parse_line(":pbkdf2-sha256:10:000102030405060708090a0b0c0d0e0f:"
                                  "f8332ea7a39557af003df934f09c9a92e648b4fe4cd432aeea7b4d017c35ab3f",
                                  &back),
             -1);
    CHECK_EQ(auth_user_parse_line("a:pbkdf2-sha256:10:000102030405060708090a0b0c0d0e0g:"
                                  "f8332ea7a39557af003df934f09c9a92e648b4fe4cd432aeea7b4d017c35ab3f",
                                  &back),
             -1);

    CHECK_EQ(auth_user_make(&u, "ronny", "", 10, &rng), -1); /* empty password refused */
    CHECK_EQ(auth_user_make(&u, "a:b", "x", 10, &rng), -1);  /* ':' in the name */
    CHECK_EQ(auth_user_make(&u, "a b", "x", 10, &rng), -1);  /* blank in the name */
    CHECK_EQ(auth_user_make(&u, "123456789012345678901234567890123", "x", 10, &rng), -1);
    CHECK_EQ(auth_user_make(&u, "ronny", "x", 10, &bad), -2);
    CHECK(auth_name_is_valid("12345678901234567890123456789012"));
}

static void verify_through_a_store(void)
{
    const AuthRandom rng = {NULL, fixed_salt};
    const AuthUserStore store = {NULL, lookup};
    CHECK_EQ(auth_user_make(&s_users[0], "ronny", "secret", 10, &rng), 0);
    CHECK_EQ(auth_user_make(&s_users[1], "guest", "other", 10, &rng), 0);
    CHECK(auth_user_verify(&store, "ronny", "secret"));
    CHECK(auth_user_verify(&store, "guest", "other"));
    CHECK(!auth_user_verify(&store, "ronny", "other")); /* another user's password */
    CHECK(!auth_user_verify(&store, "RONNY", "secret")); /* names are exact */
    CHECK(!auth_user_verify(&store, "nobody", "secret")); /* unknown user */
    CHECK(!auth_user_verify(&store, "", "secret"));
}

static void login_dialogue(void)
{
    const AuthUserStore store = {NULL, lookup};
    AuthLogin l;
    uint32_t now = 1000;

    CHECK_EQ(auth_login_init(&l, &store, 3, 1000), 0);
    CHECK(strcmp(auth_login_prompt(&l), "login: ") == 0);
    CHECK(auth_login_echo(&l));
    CHECK_EQ(auth_login_line(&l, "", now), AUTH_LOGIN_NAME); /* empty name: ask again */
    CHECK_EQ(l.failures, 0);

    /* Try 1: wrong password. */
    CHECK_EQ(auth_login_line(&l, "ronny", now), AUTH_LOGIN_PASSWORD);
    CHECK(strcmp(auth_login_prompt(&l), "password: ") == 0);
    CHECK(!auth_login_echo(&l)); /* no echo for the password */
    CHECK_EQ(auth_login_line(&l, "wrong", now), AUTH_LOGIN_WAIT);
    CHECK(auth_login_prompt(&l) == NULL);
    CHECK_EQ(auth_login_line(&l, "typed while waiting", now + 10), AUTH_LOGIN_WAIT);
    CHECK(!auth_login_tick(&l, now + 999));
    CHECK(auth_login_tick(&l, now + 1000));
    CHECK_EQ(l.state, AUTH_LOGIN_NAME);
    now += 1000;

    /* Try 2: unknown user; the dialogue asks for the password all the same. */
    CHECK_EQ(auth_login_line(&l, "nobody", now), AUTH_LOGIN_PASSWORD);
    CHECK_EQ(auth_login_line(&l, "secret", now), AUTH_LOGIN_WAIT);
    CHECK(auth_login_tick(&l, now + 1000));
    now += 1000;

    /* Try 3: a right login after two failures still works. */
    AuthLogin ok = l;
    CHECK_EQ(auth_login_line(&ok, "ronny", now), AUTH_LOGIN_PASSWORD);
    CHECK_EQ(auth_login_line(&ok, "secret", now), AUTH_LOGIN_DONE);
    CHECK(strcmp(ok.name, "ronny") == 0);
    CHECK(auth_login_prompt(&ok) == NULL);

    /* Or try 3 fails: refused, the connection is closed by the caller. */
    CHECK_EQ(auth_login_line(&l, "ronny", now), AUTH_LOGIN_PASSWORD);
    CHECK_EQ(auth_login_line(&l, "nope", now), AUTH_LOGIN_REFUSED);
    CHECK_EQ(l.failures, 3);
    CHECK_EQ(l.name[0], '\0'); /* nothing of the failed try is kept */
    CHECK(!auth_login_tick(&l, now + 5000));
    CHECK_EQ(auth_login_line(&l, "ronny", now), AUTH_LOGIN_REFUSED);

    CHECK_EQ(auth_login_init(&l, &store, 0, 1000), -1);
}

int main(void)
{
    printf("[auth]\n");
    sha256_vectors();
    hmac_vectors();
    pbkdf2_vectors();
    equal_in_constant_time();
    user_records();
    verify_through_a_store();
    login_dialogue();
    TEST_DONE();
}
