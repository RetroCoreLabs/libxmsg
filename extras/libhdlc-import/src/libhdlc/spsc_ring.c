/**************************************************************************
** SPSC_RING - lock-free byte ring for ONE writer and ONE reader         **
**                                                                       **
** Ordering, the whole reason this works without a lock:                 **
**  - the writer stores the bytes, THEN publishes in_ptr with release;   **
**  - the reader loads in_ptr with acquire, THEN reads the bytes;        **
**  - the reader frees space by publishing out_ptr with release after    **
**    copying the bytes out; the writer loads it with acquire before     **
**    overwriting.                                                       **
** Each index has exactly one writer, so no read-modify-write atomics    **
** (which the Cortex-M0+ does not have) are needed: plain atomic loads   **
** and stores only.                                                      **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#include "libhdlc/spsc_ring.h"
#include "libhdlc/hot_path.h"

#include <stddef.h>

bool spsc_ring_init(spsc_ring *r, uint8_t *storage, int size)
{
    if (r == NULL || storage == NULL || size < 2)
    {
        return false;
    }
    r->data = storage;
    r->size = size;
    r->high_water = 0;
    atomic_init(&r->in_ptr, 0);
    atomic_init(&r->out_ptr, 0);
    return true;
}

/**
 * @brief Bytes waiting between two index values.
 * @param[in] size  Storage size.
 * @param[in] in    Write index.
 * @param[in] out   Read index.
 * @return 0 .. size - 1.
 */
static HOT_PATH_CODE int used(int size, int in, int out)
{
    return (in >= out) ? (in - out) : (size - out + in);
}

/**
 * @brief Writer: copy n bytes in from in_ptr and publish the new index.
 *        The caller has made sure they fit.
 * @param[in,out] r     Ring.
 * @param[in]     data  Bytes.
 * @param[in]     n     Count.
 * @param[in]     in    Current write index (the writer's own value).
 * @param[in]     out   Read index loaded with acquire.
 */
static HOT_PATH_CODE void put(spsc_ring *r, const uint8_t *data, int n, int in, int out)
{
    int fill;
    for (int i = 0; i < n; i++)
    {
        r->data[in] = data[i];
        in++;
        if (in == r->size)
        {
            in = 0;
        }
    }
    /* Release: the bytes above are visible before the reader sees the index. */
    atomic_store_explicit(&r->in_ptr, in, memory_order_release);
    fill = used(r->size, in, out);
    if (fill > r->high_water)
    {
        r->high_water = fill;
    }
}

int HOT_PATH_CODE spsc_ring_write_some(spsc_ring *r, const uint8_t *data, int n)
{
    int in;
    int out;
    int room;

    if (n <= 0)
    {
        return 0;
    }
    in = atomic_load_explicit(&r->in_ptr, memory_order_relaxed);    /* only we write it */
    out = atomic_load_explicit(&r->out_ptr, memory_order_acquire);
    room = r->size - 1 - used(r->size, in, out);
    if (n > room)
    {
        n = room;
    }
    if (n > 0)
    {
        put(r, data, n, in, out);
    }
    return n;
}

bool HOT_PATH_CODE spsc_ring_write_block(spsc_ring *r, const uint8_t *data, int n)
{
    int in;
    int out;

    if (n < 0 || n > r->size - 1)
    {
        return false;
    }
    in = atomic_load_explicit(&r->in_ptr, memory_order_relaxed);
    out = atomic_load_explicit(&r->out_ptr, memory_order_acquire);
    if (r->size - 1 - used(r->size, in, out) < n)
    {
        return false;
    }
    if (n > 0)
    {
        put(r, data, n, in, out);
    }
    return true;
}

int HOT_PATH_CODE spsc_ring_read_block(spsc_ring *r, uint8_t *out_buf, int max)
{
    int out;
    int in;
    int count;

    if (max <= 0)
    {
        return 0;
    }
    out = atomic_load_explicit(&r->out_ptr, memory_order_relaxed);  /* only we write it */
    /* Acquire: the bytes up to in are visible once we see the index. */
    in = atomic_load_explicit(&r->in_ptr, memory_order_acquire);
    count = used(r->size, in, out);
    if (count > max)
    {
        count = max;
    }
    for (int i = 0; i < count; i++)
    {
        out_buf[i] = r->data[out];
        out++;
        if (out == r->size)
        {
            out = 0;
        }
    }
    if (count > 0)
    {
        /* Release: the bytes were copied out before the writer may reuse them. */
        atomic_store_explicit(&r->out_ptr, out, memory_order_release);
    }
    return count;
}

int spsc_ring_length(const spsc_ring *r)
{
    /* The casts drop const only for the C11 load functions, which take a
     * non-const pointer in older C libraries; nothing is written. */
    int in = atomic_load_explicit((atomic_int *)&r->in_ptr, memory_order_acquire);
    int out = atomic_load_explicit((atomic_int *)&r->out_ptr, memory_order_acquire);
    return used(r->size, in, out);
}

int spsc_ring_room(const spsc_ring *r)
{
    return r->size - 1 - spsc_ring_length(r);
}

int spsc_ring_high_water(const spsc_ring *r)
{
    return r->high_water;
}
