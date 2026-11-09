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
                       "f8332ea7a39557af003df934f09c9a92e648b4fe4cd432aeea7b4d017c35ab3f:user") == 0);
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
    const AuthUserStore store = {NULL, lookup, NULL};
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
    const AuthUserStore store = {NULL, lookup, NULL};
    AuthLogin l;
    uint32_t now = 1000;

    CHECK_EQ(auth_login_init(&l, &store, NULL, 3, 1000), 0);
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

    CHECK_EQ(auth_login_init(&l, &store, NULL, 0, 1000), -1);
}

/* ------------------------------------------------------------------ levels and changes */

#define K_SALT "000102030405060708090a0b0c0d0e0f"
#define K_HASH "f8332ea7a39557af003df934f09c9a92e648b4fe4cd432aeea7b4d017c35ab3f"

static void flags_field(void)
{
    static const struct
    {
        AuthLevel level;
        bool must_change;
        const char *flags;
    } k_all[] = {
        {AUTH_LEVEL_USER, false, "user"},
        {AUTH_LEVEL_USER, true, "user,change"},
        {AUTH_LEVEL_ADMIN, false, "admin"},
        {AUTH_LEVEL_ADMIN, true, "admin,change"},
    };
    static const char *const k_bad[] = {"", "root", "Admin", "admin,", "change",
                                        "user,change,change",
                                        "user,Change", ",change", "admin change"};
    const AuthRandom rng = {NULL, fixed_salt};
    AuthUser u;
    AuthUser back;
    char line[AUTH_LINE_MAX];
    char want[AUTH_LINE_MAX];

    /* A five-field line from before the flags: level user, no change. */
    memset(&back, 0x55, sizeof back);
    CHECK_EQ(auth_user_parse_line("ronny:pbkdf2-sha256:10:" K_SALT ":" K_HASH, &back), 0);
    CHECK_EQ(back.level, AUTH_LEVEL_USER);
    CHECK(!back.must_change);
    CHECK(auth_user_check(&back, "secret"));

    CHECK_EQ(auth_user_make(&u, "ronny", "secret", 10, &rng), 0);
    CHECK_EQ(u.level, AUTH_LEVEL_USER);
    CHECK(!u.must_change);
    for (size_t i = 0; i < sizeof k_all / sizeof k_all[0]; i++)
    {
        u.level = k_all[i].level;
        u.must_change = k_all[i].must_change;
        CHECK(auth_user_format_line(&u, line, sizeof line) > 0);
        snprintf(want, sizeof want, "ronny:pbkdf2-sha256:10:" K_SALT ":" K_HASH ":%s",
                 k_all[i].flags);
        CHECK(strcmp(line, want) == 0);
        CHECK_EQ(auth_user_parse_line(line, &back), 0);
        CHECK(memcmp(&back, &u, sizeof u) == 0); /* round trip */
        CHECK_EQ(back.level, k_all[i].level);
        CHECK_EQ(back.must_change, k_all[i].must_change);
    }
    for (size_t i = 0; i < sizeof k_bad / sizeof k_bad[0]; i++)
    {
        snprintf(want, sizeof want, "ronny:pbkdf2-sha256:10:" K_SALT ":" K_HASH ":%s", k_bad[i]);
        CHECK_EQ(auth_user_parse_line(want, &back), -1);
    }
    CHECK_EQ(auth_user_parse_line("ronny:pbkdf2-sha256:10:" K_SALT ":" K_HASH ":user:x", &back),
             -1); /* a seventh field */

    /* The longest line fits AUTH_LINE_MAX. */
    memset(&u, 0, sizeof u);
    memset(u.name, 'n', AUTH_NAME_MAX);
    u.iterations = UINT32_MAX;
    u.level = AUTH_LEVEL_ADMIN;
    u.must_change = true;
    CHECK(auth_user_format_line(&u, line, sizeof line) > 0);
    u.level = (AuthLevel)7;
    CHECK_EQ(auth_user_format_line(&u, line, sizeof line), -1);
}

static void verify_get_returns_the_record(void)
{
    const AuthRandom rng = {NULL, fixed_salt};
    const AuthUserStore store = {NULL, lookup, NULL};
    AuthUser got;
    CHECK_EQ(auth_user_make(&s_users[0], "ronny", "secret", 10, &rng), 0);
    s_users[0].level = AUTH_LEVEL_ADMIN;
    s_users[0].must_change = true;
    CHECK_EQ(auth_user_make(&s_users[1], "guest", "other", 10, &rng), 0);
    CHECK(auth_user_verify_get(&store, "ronny", "secret", &got));
    CHECK(memcmp(&got, &s_users[0], sizeof got) == 0);
    CHECK_EQ(got.level, AUTH_LEVEL_ADMIN);
    CHECK(got.must_change);
    memset(&got, 0x55, sizeof got);
    CHECK(!auth_user_verify_get(&store, "ronny", "wrong", &got));
    CHECK_EQ(got.name[0] | got.hash[0] | (int)got.level, 0); /* zeroed on failure */
    CHECK(!auth_user_verify_get(&store, "nobody", "secret", &got));
    CHECK(!auth_user_verify_get(&store, "ronny", "secret", NULL));
}

static void change_and_reset(void)
{
    const AuthRandom rng = {NULL, fixed_salt};
    const AuthRandom bad = {NULL, broken_random};
    AuthUser u;
    AuthUser before;
    AuthUser admins[3];

    CHECK_EQ(auth_user_make(&u, "ronny", "secret", 10, &rng), 0);
    u.must_change = true;
    before = u;
    CHECK_EQ(auth_user_change_password(&u, "wrong", "new1", &rng), AUTH_CHANGE_WRONG_OLD);
    CHECK_EQ(auth_user_change_password(&u, "secret", "", &rng), AUTH_CHANGE_EMPTY);
    CHECK_EQ(auth_user_change_password(&u, "secret", "secret", &rng), AUTH_CHANGE_SAME);
    CHECK_EQ(auth_user_change_password(&u, "secret", "new1", &bad), AUTH_CHANGE_RANDOM);
    CHECK(memcmp(&u, &before, sizeof u) == 0); /* failures leave the record alone */
    CHECK_EQ(auth_user_change_password(&u, "secret", "new1", &rng), AUTH_CHANGE_OK);
    CHECK(auth_user_check(&u, "new1"));
    CHECK(!auth_user_check(&u, "secret"));
    CHECK(!u.must_change);
    CHECK(strcmp(u.name, "ronny") == 0);
    CHECK_EQ(u.iterations, 10);

    /* Admin reset: new password, must_change set, level kept. */
    u.level = AUTH_LEVEL_ADMIN;
    CHECK_EQ(auth_user_reset_password(&u, "", &rng), AUTH_CHANGE_EMPTY);
    CHECK_EQ(auth_user_reset_password(&u, "temp", &bad), AUTH_CHANGE_RANDOM);
    CHECK(!u.must_change);
    CHECK_EQ(auth_user_reset_password(&u, "temp", &rng), AUTH_CHANGE_OK);
    CHECK(u.must_change);
    CHECK(auth_user_check(&u, "temp"));
    CHECK(!auth_user_check(&u, "new1"));
    CHECK_EQ(u.level, AUTH_LEVEL_ADMIN);

    /* The default admin. */
    CHECK_EQ(auth_user_make_default_admin(&u, 10, &rng), 0);
    CHECK(strcmp(u.name, "admin") == 0);
    CHECK(auth_user_check(&u, "admin"));
    CHECK_EQ(u.level, AUTH_LEVEL_ADMIN);
    CHECK(u.must_change);
    CHECK_EQ(u.iterations, 10);
    CHECK_EQ(auth_user_make_default_admin(&u, 0, &rng), -1);

    /* The last admin. */
    memset(admins, 0, sizeof admins);
    snprintf(admins[0].name, sizeof admins[0].name, "a");
    snprintf(admins[1].name, sizeof admins[1].name, "b");
    snprintf(admins[2].name, sizeof admins[2].name, "c");
    admins[0].level = AUTH_LEVEL_ADMIN;
    CHECK(auth_users_is_last_admin(admins, 3, "a"));
    CHECK(!auth_users_is_last_admin(admins, 3, "b")); /* not an admin */
    CHECK(!auth_users_is_last_admin(admins, 3, "x")); /* not there */
    admins[2].level = AUTH_LEVEL_ADMIN;
    CHECK(!auth_users_is_last_admin(admins, 3, "a")); /* another admin left */
    CHECK(!auth_users_is_last_admin(NULL, 0, "a"));
}

/* A store whose save writes back into s_users and counts. */
static int s_saves;
static int s_save_result;

static int save(void *ctx, const AuthUser *u)
{
    (void)ctx;
    s_saves++;
    if (s_save_result != 0)
    {
        return s_save_result;
    }
    for (int i = 0; i < N_USERS; i++)
    {
        if (strcmp(s_users[i].name, u->name) == 0)
        {
            s_users[i] = *u;
            return 0;
        }
    }
    return -1;
}

static void must_change_login(void)
{
    const AuthRandom rng = {NULL, fixed_salt};
    const AuthUserStore store = {NULL, lookup, save};
    const AuthUserStore no_save = {NULL, lookup, NULL};
    AuthLogin l;
    AuthLogin start;
    const AuthUser *u;

    CHECK_EQ(auth_user_make(&s_users[0], "ronny", "secret", 10, &rng), 0);
    s_users[0].must_change = true;
    s_users[0].level = AUTH_LEVEL_ADMIN;
    CHECK_EQ(auth_user_make(&s_users[1], "guest", "other", 10, &rng), 0);
    s_saves = 0;
    s_save_result = 0;

    CHECK_EQ(auth_login_init(&l, &store, &rng, 3, 1000), 0);
    CHECK_EQ(auth_login_line(&l, "ronny", 0), AUTH_LOGIN_PASSWORD);
    CHECK_EQ(auth_login_line(&l, "secret", 0), AUTH_LOGIN_NEW_PASSWORD);
    CHECK(auth_login_must_change(&l));
    CHECK_EQ(auth_login_level(&l), AUTH_LEVEL_USER); /* not logged in yet */
    CHECK(auth_login_user(&l) == NULL);
    CHECK(strcmp(auth_login_prompt(&l), "new password: ") == 0);
    CHECK(!auth_login_echo(&l));
    start = l;

    /* Empty, and the same as the old: refused, asked again. */
    CHECK_EQ(auth_login_line(&l, "", 0), AUTH_LOGIN_NEW_PASSWORD);
    CHECK_EQ(auth_login_take_note(&l), AUTH_NOTE_EMPTY);
    CHECK_EQ(auth_login_line(&l, "secret", 0), AUTH_LOGIN_NEW_PASSWORD);
    CHECK_EQ(auth_login_take_note(&l), AUTH_NOTE_SAME);
    /* Mismatch: back to the first entry. */
    CHECK_EQ(auth_login_line(&l, "fresh", 0), AUTH_LOGIN_NEW_AGAIN);
    CHECK(strcmp(auth_login_prompt(&l), "again: ") == 0);
    CHECK(!auth_login_echo(&l));
    CHECK_EQ(auth_login_line(&l, "fresh2", 0), AUTH_LOGIN_NEW_PASSWORD);
    CHECK_EQ(auth_login_take_note(&l), AUTH_NOTE_MISMATCH);
    CHECK_EQ(s_saves, 0);
    /* Right: new twice, saved, DONE. */
    CHECK_EQ(auth_login_line(&l, "fresh", 0), AUTH_LOGIN_NEW_AGAIN);
    CHECK_EQ(auth_login_line(&l, "fresh", 0), AUTH_LOGIN_DONE);
    CHECK_EQ(auth_login_take_note(&l), AUTH_NOTE_CHANGED);
    CHECK_EQ(s_saves, 1);
    u = auth_login_user(&l);
    CHECK(u != NULL);
    CHECK(auth_user_check(u, "fresh"));
    CHECK(!auth_user_check(u, "secret"));
    CHECK(!u->must_change);
    CHECK_EQ(u->level, AUTH_LEVEL_ADMIN);
    CHECK_EQ(auth_login_level(&l), AUTH_LEVEL_ADMIN);
    CHECK(!auth_login_must_change(&l));
    CHECK(memcmp(&s_users[0], u, sizeof *u) == 0); /* the store got the same record */

    /* A save that fails ends the forced change: refused. */
    l = start;
    s_save_result = -1;
    CHECK_EQ(auth_login_line(&l, "x1", 0), AUTH_LOGIN_NEW_AGAIN);
    CHECK_EQ(auth_login_line(&l, "x1", 0), AUTH_LOGIN_REFUSED);
    CHECK_EQ(auth_login_take_note(&l), AUTH_NOTE_SAVE_FAILED);
    s_save_result = 0;

    /* No save in the store: the user who must change cannot log in. */
    s_users[0].must_change = true;
    CHECK_EQ(auth_login_init(&l, &no_save, &rng, 3, 1000), 0);
    CHECK_EQ(auth_login_line(&l, "ronny", 0), AUTH_LOGIN_PASSWORD);
    CHECK_EQ(auth_login_line(&l, "fresh", 0), AUTH_LOGIN_REFUSED);
    CHECK_EQ(auth_login_take_note(&l), AUTH_NOTE_NO_SAVE);
    CHECK(strlen(auth_login_note_text(AUTH_NOTE_NO_SAVE)) > 0);
    /* ... nor with no random source. */
    CHECK_EQ(auth_login_init(&l, &store, NULL, 3, 1000), 0);
    CHECK_EQ(auth_login_line(&l, "ronny", 0), AUTH_LOGIN_PASSWORD);
    CHECK_EQ(auth_login_line(&l, "fresh", 0), AUTH_LOGIN_REFUSED);
    s_users[0].must_change = false;

    /* A plain user logs in straight away, level user. */
    CHECK_EQ(auth_login_init(&l, &store, &rng, 3, 1000), 0);
    CHECK_EQ(auth_login_line(&l, "guest", 0), AUTH_LOGIN_PASSWORD);
    CHECK_EQ(auth_login_line(&l, "other", 0), AUTH_LOGIN_DONE);
    CHECK_EQ(auth_login_level(&l), AUTH_LEVEL_USER);

    /* Voluntary change: old, new, again. */
    start = l;
    CHECK_EQ(auth_login_change_begin(&l), 0);
    CHECK(strcmp(auth_login_prompt(&l), "old password: ") == 0);
    CHECK(!auth_login_echo(&l));
    CHECK_EQ(auth_login_line(&l, "wrong", 0), AUTH_LOGIN_DONE);
    CHECK_EQ(auth_login_take_note(&l), AUTH_NOTE_WRONG_OLD);
    CHECK_EQ(auth_login_change_begin(&l), 0);
    CHECK_EQ(auth_login_line(&l, "other", 0), AUTH_LOGIN_NEW_PASSWORD);
    CHECK_EQ(auth_login_level(&l), AUTH_LEVEL_USER);
    CHECK_EQ(auth_login_line(&l, "other", 0), AUTH_LOGIN_NEW_PASSWORD);
    CHECK_EQ(auth_login_take_note(&l), AUTH_NOTE_SAME);
    CHECK_EQ(auth_login_line(&l, "guest2", 0), AUTH_LOGIN_NEW_AGAIN);
    s_save_result = -1; /* a failed save keeps the old record and stays logged in */
    CHECK_EQ(auth_login_line(&l, "guest2", 0), AUTH_LOGIN_DONE);
    CHECK_EQ(auth_login_take_note(&l), AUTH_NOTE_SAVE_FAILED);
    CHECK(auth_user_check(auth_login_user(&l), "other"));
    s_save_result = 0;
    CHECK_EQ(auth_login_change_begin(&l), 0);
    CHECK_EQ(auth_login_line(&l, "other", 0), AUTH_LOGIN_NEW_PASSWORD);
    CHECK_EQ(auth_login_line(&l, "guest2", 0), AUTH_LOGIN_NEW_AGAIN);
    CHECK_EQ(auth_login_line(&l, "guest2", 0), AUTH_LOGIN_DONE);
    CHECK_EQ(auth_login_take_note(&l), AUTH_NOTE_CHANGED);
    CHECK(auth_user_check(&s_users[1], "guest2"));
    CHECK(!auth_user_check(&s_users[1], "other"));

    /* No change possible without a save. */
    start.store.save = NULL;
    CHECK_EQ(auth_login_change_begin(&start), -1);
    CHECK_EQ(auth_login_take_note(&start), AUTH_NOTE_NO_SAVE);
    CHECK_EQ(start.state, AUTH_LOGIN_DONE);
}

/* A hash port that counts its calls and hands them to the built-in function. */
static int s_port_calls;

static int counting_pbkdf2(void *ctx, const uint8_t *pw, size_t pw_len, const uint8_t *salt,
                           size_t salt_len, uint32_t iterations, uint8_t *out, size_t out_len)
{
    (void)ctx;
    s_port_calls++;
    return auth_pbkdf2_sha256(pw, pw_len, salt, salt_len, iterations, out, out_len);
}

static int failing_pbkdf2(void *ctx, const uint8_t *pw, size_t pw_len, const uint8_t *salt,
                          size_t salt_len, uint32_t iterations, uint8_t *out, size_t out_len)
{
    (void)ctx;
    (void)pw;
    (void)pw_len;
    (void)salt;
    (void)salt_len;
    (void)iterations;
    (void)out;
    (void)out_len;
    s_port_calls++;
    return 5;
}

static void hash_port(void)
{
    const AuthRandom rng = {NULL, fixed_salt};
    const AuthUserStore store = {NULL, lookup, save};
    const AuthHashPort counting = {NULL, counting_pbkdf2};
    const AuthHashPort failing = {NULL, failing_pbkdf2};
    AuthUser u;
    AuthUser got;
    AuthLogin l;

    s_port_calls = 0;
    auth_hash_set_port(&counting);
    CHECK_EQ(auth_user_make(&u, "ronny", "secret", 10, &rng), 0);
    CHECK_EQ(s_port_calls, 1);
    /* The same result as the built-in function (the vector of user_records). */
    CHECK_BYTES(u.hash, 32, "f8332ea7a39557af003df934f09c9a92e648b4fe4cd432aeea7b4d017c35ab3f");
    CHECK(auth_user_check(&u, "secret"));
    CHECK_EQ(s_port_calls, 2);
    s_users[0] = u;
    CHECK(auth_user_verify(&store, "ronny", "secret"));
    CHECK_EQ(s_port_calls, 3);
    CHECK(!auth_user_verify_get(&store, "nobody", "secret", &got)); /* the equal-time run too */
    CHECK_EQ(s_port_calls, 4);
    CHECK_EQ(auth_user_change_password(&u, "secret", "next", &rng), AUTH_CHANGE_OK);
    CHECK_EQ(s_port_calls, 7); /* old check, same-check, new hash */
    CHECK_EQ(auth_user_reset_password(&u, "temp", &rng), AUTH_CHANGE_OK);
    CHECK_EQ(s_port_calls, 8);
    CHECK_EQ(auth_login_init(&l, &store, &rng, 3, 0), 0);
    CHECK_EQ(auth_login_line(&l, "ronny", 0), AUTH_LOGIN_PASSWORD);
    CHECK_EQ(auth_login_line(&l, "secret", 0), AUTH_LOGIN_DONE);
    CHECK_EQ(s_port_calls, 9);

    /* A failing port makes every check fail; nothing falls back to the built-in one. */
    auth_hash_set_port(&failing);
    CHECK(!auth_user_check(&u, "temp"));
    CHECK_EQ(auth_user_make(&got, "ronny", "secret", 10, &rng), -1);
    CHECK_EQ(s_port_calls, 11);

    auth_hash_set_port(NULL); /* back to the built-in */
    CHECK(auth_user_check(&u, "temp"));
    CHECK_EQ(s_port_calls, 11);
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
    flags_field();
    verify_get_returns_the_record();
    change_and_reset();
    must_change_login();
    hash_port();
    TEST_DONE();
}
