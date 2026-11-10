/*
 * test_ring_buffer.c - the locked byte ring: block writes, wrap, high water, and two threads.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * The two-thread test runs the same lock the library uses on this host (SRWLOCK on Windows,
 * pthread elsewhere). It pushes a counting byte pattern through a small ring and checks that the
 * reader sees every byte once and in order.
 */
#include "libhdlc/ring_buffer.h"
#include "test_util.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

static void basics_and_wrap(void)
{
    ring_buffer rb;
    uint8_t out[16];
    static const uint8_t a[] = { 1, 2, 3, 4, 5 };

    CHECK(!ring_buffer_alloc(&rb, 1));
    CHECK(ring_buffer_alloc(&rb, 8));
    CHECK(ring_buffer_is_empty(&rb));
    CHECK_EQ(ring_buffer_size(&rb), 8);

    CHECK(ring_buffer_write_block(&rb, a, 5));
    CHECK_EQ(ring_buffer_length(&rb), 5);
    CHECK(!ring_buffer_write_block(&rb, a, 5));     /* all or nothing */
    CHECK_EQ(ring_buffer_length(&rb), 5);
    CHECK(!ring_buffer_write_block(&rb, a, 8));     /* never fits, size - 1 is the most */

    CHECK_EQ(ring_buffer_read_block(&rb, out, 3), 3);
    CHECK_BYTES(out, 3, "01 02 03");

    /* Wraps: 2 waiting, write 5 more across the end. */
    CHECK(ring_buffer_write_block(&rb, a, 5));
    CHECK_EQ(ring_buffer_length(&rb), 7);
    CHECK_EQ(ring_buffer_write_some(&rb, a, 5), 1); /* only one byte of room left */
    CHECK(ring_buffer_is_full(&rb));
    CHECK_EQ(ring_buffer_high_water(&rb), 8);

    CHECK_EQ(ring_buffer_read_block(&rb, out, 16), 8);
    CHECK_BYTES(out, 8, "04 05 01 02 03 04 05 01");
    CHECK(ring_buffer_is_empty(&rb));
    CHECK_EQ(ring_buffer_read(&rb), 0);

    ring_buffer_write(&rb, 0x99);
    CHECK_EQ(ring_buffer_read(&rb), 0x99);

    ring_buffer_write(&rb, 0x11);
    ring_buffer_clear(&rb);
    CHECK(ring_buffer_is_empty(&rb));
    CHECK_EQ(ring_buffer_high_water(&rb), 8);       /* clear keeps the measurement */
    ring_buffer_free(&rb);
}

static void caller_storage(void)
{
    uint8_t store[4];
    ring_buffer rb;
    CHECK(!ring_buffer_init(&rb, NULL, 4));
    CHECK(ring_buffer_init(&rb, store, 4));
    ring_buffer_write(&rb, 7);
    CHECK_EQ(store[0], 7);
    ring_buffer_free(&rb);   /* must not free store */
}

/* ------------------------------------------------------------ threads */

#define TOTAL_BYTES 1000000

static ring_buffer s_rb;

#ifdef _WIN32
static DWORD WINAPI producer(LPVOID arg)
#else
static void *producer(void *arg)
#endif
{
    uint8_t chunk[37];
    int sent = 0;
    (void)arg;
    while (sent < TOTAL_BYTES)
    {
        int n = TOTAL_BYTES - sent;
        n = (n < (int)sizeof chunk) ? n : (int)sizeof chunk;
        for (int i = 0; i < n; i++)
        {
            chunk[i] = (uint8_t)(sent + i);
        }
        sent += ring_buffer_write_some(&s_rb, chunk, n);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void two_threads_in_order(void)
{
    uint8_t buf[29];
    int got = 0;
    int wrong = 0;

    CHECK(ring_buffer_alloc(&s_rb, 64));
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 0, producer, NULL, 0, NULL);
    CHECK(t != NULL);
#else
    pthread_t t;
    CHECK(pthread_create(&t, NULL, producer, NULL) == 0);
#endif
    while (got < TOTAL_BYTES)
    {
        int n = ring_buffer_read_block(&s_rb, buf, (int)sizeof buf);
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
    CHECK_EQ(got, TOTAL_BYTES);
    CHECK_EQ(wrong, 0);
    CHECK(ring_buffer_is_empty(&s_rb));
    ring_buffer_free(&s_rb);
}

int main(void)
{
    basics_and_wrap();
    caller_storage();
    two_threads_in_order();
    TEST_DONE();
}
