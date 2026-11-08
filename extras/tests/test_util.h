/*
 * test_util.h - the whole unit-test harness: a handful of CHECK macros and a verdict line.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * No framework, as in nd100x: each test_<module>.c is its own executable, calls CHECK* in plain
 * functions, and ends with TEST_DONE(). CTest looks for the "TB_RESULT: PASS" line, so a test that
 * crashes before printing it fails even if its exit code were lost. This is test code, so printf
 * is allowed here (house rule 9.4 is about library code).
 */
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_test_failures = 0;
static int s_test_checks = 0;

/** Record one check. Prints only on failure, with file and line. */
#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        s_test_checks++;                                                                           \
        if (!(cond))                                                                               \
        {                                                                                          \
            s_test_failures++;                                                                     \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                               \
        }                                                                                          \
    } while (0)

/** Compare two integers, printing both on failure. */
#define CHECK_EQ(actual, expected)                                                                 \
    do                                                                                             \
    {                                                                                              \
        long long test_a_ = (long long)(actual);                                                   \
        long long test_e_ = (long long)(expected);                                                 \
        s_test_checks++;                                                                           \
        if (test_a_ != test_e_)                                                                    \
        {                                                                                          \
            s_test_failures++;                                                                     \
            printf("  FAIL %s:%d: %s = %lld (0x%llX), expected %lld (0x%llX)\n", __FILE__,         \
                   __LINE__, #actual, test_a_, (unsigned long long)test_a_, test_e_,               \
                   (unsigned long long)test_e_);                                                   \
        }                                                                                          \
    } while (0)

/**
 * @brief Compare a byte buffer with a hex string such as "7E 01 3F 00 64 2E 09 7E".
 *
 * Spaces in the string are ignored. On failure both sides are printed as hex so the first
 * differing byte can be seen. Returns through the shared counters like the other macros.
 */
#define CHECK_BYTES(actual, actual_len, expected_hex)                                              \
    test_check_bytes(__FILE__, __LINE__, (actual), (size_t)(actual_len), (expected_hex))

/** Parse "7E 01 3F" into bytes. Returns the count, or -1 when the text is not hex pairs. */
static int test_hex_to_bytes(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = 0;
    int nibbles = 0;
    uint8_t acc = 0;
    for (const char *p = hex; *p != '\0'; p++)
    {
        int v;
        if (*p == ' ' || *p == '\n' || *p == '\t')
        {
            continue;
        }
        if (*p >= '0' && *p <= '9')
        {
            v = *p - '0';
        }
        else if (*p >= 'a' && *p <= 'f')
        {
            v = *p - 'a' + 10;
        }
        else if (*p >= 'A' && *p <= 'F')
        {
            v = *p - 'A' + 10;
        }
        else
        {
            return -1;
        }
        acc = (uint8_t)((acc << 4) | (uint8_t)v);
        nibbles++;
        if (nibbles == 2)
        {
            if (n >= cap)
            {
                return -1;
            }
            out[n++] = acc;
            acc = 0;
            nibbles = 0;
        }
    }
    if (nibbles != 0)
    {
        return -1;
    }
    return (int)n;
}

static void test_print_hex(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        printf("%s%02X", (i != 0) ? " " : "", b[i]);
    }
}

static void test_check_bytes(const char *file, int line, const uint8_t *actual, size_t actual_len,
                             const char *expected_hex)
{
    uint8_t expected[4096];
    int expected_len = test_hex_to_bytes(expected_hex, expected, sizeof expected);
    s_test_checks++;
    if (expected_len < 0)
    {
        s_test_failures++;
        printf("  FAIL %s:%d: expected string is not hex: \"%s\"\n", file, line, expected_hex);
        return;
    }
    if ((size_t)expected_len != actual_len || memcmp(actual, expected, actual_len) != 0)
    {
        s_test_failures++;
        printf("  FAIL %s:%d: bytes differ\n    actual   (%u): ", file, line, (unsigned)actual_len);
        test_print_hex(actual, actual_len);
        printf("\n    expected (%u): ", (unsigned)expected_len);
        test_print_hex(expected, (size_t)expected_len);
        printf("\n");
    }
}

/** Print the verdict CTest looks for and return the process exit code. */
#define TEST_DONE()                                                                                \
    do                                                                                             \
    {                                                                                              \
        printf("%d checks, %d failed\n", s_test_checks, s_test_failures);                          \
        printf("TB_RESULT: %s\n", (s_test_failures == 0) ? "PASS" : "FAIL");                       \
        return (s_test_failures == 0) ? 0 : 1;                                                     \
    } while (0)

#endif /* TEST_UTIL_H */
