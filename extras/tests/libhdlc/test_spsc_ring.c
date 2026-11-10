/*
 * test_spsc_ring.c - the lock-free one-writer/one-reader ring.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * The thread test runs a real writer thread and a real reader (this thread) against a small
 * ring, mixing block writes and partial writes, and checks every byte arrives once, in order,
 * and that a block is never seen half written. On x86 the hardware orders memory more strongly
 * than the Pico's Cortex-M0+, so this test cannot prove the barriers are right there; the
 * ordering argument is in spsc_ring.c and the code uses only C11 release/acquire for it.
 */
#include "libhdlc/spsc_ring.h"
#include "test_util.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

static void basics(void)
{
    uint8_t store[8];
    uint8_t out[16];
    spsc_ring r;
    static const uint8_t a[] = { 1, 2, 3, 4, 5 };

    CHECK(!spsc_ring_init(&r, store, 1));
    CHECK(spsc_ring_init(&r, store, 8));
    CHECK_EQ(spsc_ring_length(&r), 0);
    CHECK_EQ(spsc_ring_room(&r), 7);

    CHECK(spsc_ring_write_block(&r, a, 5));
    CHECK(!spsc_ring_write_block(&r, a, 5));      /* all or nothing */
    CHECK(!spsc_ring_write_block(&r, a, 8));      /* never fits */
    CHECK_EQ(spsc_ring_length(&r), 5);
    CHECK_EQ(spsc_ring_read_block(&r, out, 3), 3);
    CHECK_BYTES(out, 3, "01 02 03");

    CHECK(spsc_ring_write_block(&r, a, 5));       /* wraps */
    CHECK_EQ(spsc_ring_length(&r), 7);
    CHECK_EQ(spsc_ring_write_some(&r, a, 5), 0);  /* full: holds size - 1 */
    CHECK_EQ(spsc_ring_high_water(&r), 7);
    CHECK_EQ(spsc_ring_read_block(&r, out, 16), 7);
    CHECK_BYTES(out, 7, "04 05 01 02 03 04 05");
    CHECK_EQ(spsc_ring_read_block(&r, out, 16), 0);

    CHECK_EQ(spsc_ring_write_some(&r, a, 5), 5);
    CHECK_EQ(spsc_ring_write_some(&r, a, 5), 2);
    CHECK_EQ(spsc_ring_room(&r), 0);
}

/* ------------------------------------------------------------ threads */

#define TOTAL 2000000
#define BLOCK 13

static spsc_ring s_ring;
static uint8_t s_store[97];

#ifdef _WIN32
static DWORD WINAPI writer(LPVOID arg)
#else
static void *writer(void *arg)
#endif
{
    uint8_t chunk[BLOCK];
    int sent = 0;
    int turn = 0;
    (void)arg;
    while (sent < TOTAL)
    {
        int n = TOTAL - sent;
        n = (n < BLOCK) ? n : BLOCK;
        for (int i = 0; i < n; i++)
        {
            chunk[i] = (uint8_t)(sent + i);
        }
        if ((turn++ & 1) != 0 && n == BLOCK)
        {
            if (spsc_ring_write_block(&s_ring, chunk, n))
            {
                sent += n;
            }
        }
        else
        {
            sent += spsc_ring_write_some(&s_ring, chunk, n);
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void two_threads_in_order(void)
{
    uint8_t buf[31];
    int got = 0;
    int wrong = 0;

    CHECK(spsc_ring_init(&s_ring, s_store, (int)sizeof s_store));
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 0, writer, NULL, 0, NULL);
    CHECK(t != NULL);
#else
    pthread_t t;
    CHECK(pthread_create(&t, NULL, writer, NULL) == 0);
#endif
    while (got < TOTAL)
    {
        int n = spsc_ring_read_block(&s_ring, buf, (int)sizeof buf);
        for (int i = 0; i < n; i++)
        {
            if (buf[i] != (uint8_t)(got + i))
            {
                wrong++;
            }
        }
        got += n;
    }
#ifdef _WIN32
    (void)WaitForSingleObject(t, INFINITE);
    (void)CloseHandle(t);
#else
    (void)pthread_join(t, NULL);
#endif
    CHECK_EQ(got, TOTAL);
    CHECK_EQ(wrong, 0);
    CHECK_EQ(spsc_ring_length(&s_ring), 0);
    CHECK(spsc_ring_high_water(&s_ring) <= (int)sizeof s_store - 1);
}

int main(void)
{
    basics();
    two_threads_in_order();
    TEST_DONE();
}
