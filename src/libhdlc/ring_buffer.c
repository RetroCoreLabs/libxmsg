/**************************************************************************
** RING_BUFFER - byte ring buffer shared between tasks or threads       **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen       **
***************************************************************************/
/* On the Raspberry Pi Pico this file compiles to nothing: there is no lock for it yet (BUGS.md
 * L-PICO), and ring_buffer.h stops any code that tries to USE the ring there. The rest of the
 * library still builds for the Pico. */
#if !(defined(ARDUINO_ARCH_RP2040) || defined(PICO_BUILD))

#include "libhdlc/ring_buffer.h"
#include "libhdlc/hot_path.h"

#include <stdlib.h>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

/* ------------------------------------------------------- platform lock */

/**
 * @brief Create the mutex of a buffer.
 * @param[out] lock  Lock to create.
 * @return true when the lock is ready.
 */
static bool lock_create(ring_buffer_lock *lock)
{
#if defined(ESP_PLATFORM)
    *lock = xSemaphoreCreateMutex();
    return *lock != NULL;
#elif defined(_WIN32)
    InitializeSRWLock(lock);
    return true;
#else
    return pthread_mutex_init(lock, NULL) == 0;
#endif
}

/**
 * @brief Destroy a mutex made by lock_create().
 * @param[in,out] lock  Lock to destroy.
 */
static void lock_destroy(ring_buffer_lock *lock)
{
#if defined(ESP_PLATFORM)
    vSemaphoreDelete(*lock);
    *lock = NULL;
#elif defined(_WIN32)
    (void)lock;     /* an SRWLOCK holds no resources */
#else
    /* Fails only when the mutex is held; ring_buffer_free() forbids that. */
    (void)pthread_mutex_destroy(lock);
#endif
}

/**
 * @brief Take the mutex, waiting as long as it takes.
 * @param[in,out] lock  Lock.
 * @return true when held. Only the FreeRTOS and pthread calls can report a
 *         failure; the caller then leaves the buffer untouched.
 */
static HOT_PATH_CODE bool lock_take(ring_buffer_lock *lock)
{
#if defined(ESP_PLATFORM)
    return xSemaphoreTake(*lock, portMAX_DELAY) == pdTRUE;
#elif defined(_WIN32)
    AcquireSRWLockExclusive(lock);
    return true;
#else
    return pthread_mutex_lock(lock) == 0;
#endif
}

/**
 * @brief Release the mutex taken by lock_take().
 * @param[in,out] lock  Lock.
 */
static HOT_PATH_CODE void lock_give(ring_buffer_lock *lock)
{
#if defined(ESP_PLATFORM)
    /* Fails only when the mutex is not held, which lock_take() rules out. */
    (void)xSemaphoreGive(*lock);
#elif defined(_WIN32)
    ReleaseSRWLockExclusive(lock);
#else
    (void)pthread_mutex_unlock(lock);
#endif
}

/* ------------------------------------------------------------- storage */

/**
 * @brief Hand allocated storage to a buffer, or free it when init fails.
 * @param[out] rb       Buffer.
 * @param[in]  storage  Allocated storage, may be NULL.
 * @param[in]  size     Capacity.
 * @return true when the buffer is ready.
 */
static bool init_owned(ring_buffer *rb, uint8_t *storage, int size)
{
    if (storage == NULL)
    {
        return false;
    }
    if (!ring_buffer_init(rb, storage, size))
    {
        free(storage);
        return false;
    }
    rb->owns_storage = true;
    return true;
}

bool ring_buffer_alloc(ring_buffer *rb, int size)
{
    uint8_t *storage;

    if (size < 2)
    {
        return false;
    }
#ifdef ESP_PLATFORM
    /* heap_caps_malloc() memory is released with plain free() (ESP-IDF heap
     * documentation), so ring_buffer_free() needs no ESP branch. */
    storage = (uint8_t *)heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM);
    if (storage == NULL)
    {
        storage = (uint8_t *)malloc((size_t)size);
    }
#else
    storage = (uint8_t *)malloc((size_t)size);
#endif
    return init_owned(rb, storage, size);
}

bool ring_buffer_alloc_internal(ring_buffer *rb, int size)
{
    uint8_t *storage;

    if (size < 2)
    {
        return false;
    }
#ifdef ESP_PLATFORM
    storage = (uint8_t *)heap_caps_malloc((size_t)size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    storage = (uint8_t *)malloc((size_t)size);
#endif
    return init_owned(rb, storage, size);
}

void ring_buffer_free(ring_buffer *rb)
{
    lock_destroy(&rb->mutex);
    if (rb->owns_storage)
    {
        free(rb->data);
    }
    rb->data = NULL;
    rb->size = 0;
    rb->owns_storage = false;
}

int ring_buffer_high_water(const ring_buffer *rb)
{
    return rb->high_water;
}

int ring_buffer_size(const ring_buffer *rb)
{
    return rb->size;
}

bool ring_buffer_init(ring_buffer *rb, uint8_t *storage, int size)
{
    if (rb == NULL || storage == NULL || size < 2)
    {
        return false;
    }
    if (!lock_create(&rb->mutex))
    {
        return false;
    }
    rb->data = storage;
    rb->size = size;
    rb->owns_storage = false;
    rb->high_water = 0;
    ring_buffer_clear(rb);
    return true;
}

void ring_buffer_clear(ring_buffer *rb)
{
    if (lock_take(&rb->mutex))
    {
        rb->in_ptr = 0;
        rb->out_ptr = 0;
        rb->empty = true;
        rb->full = false;
        lock_give(&rb->mutex);
    }
}

/**
 * @brief Bytes waiting, computed with the mutex already held.
 * @param[in] rb  Buffer.
 * @return 0 .. size.
 */
static HOT_PATH_CODE int length_locked(const ring_buffer *rb)
{
    if (rb->full)
    {
        return rb->size;
    }
    if (rb->in_ptr >= rb->out_ptr)
    {
        return rb->in_ptr - rb->out_ptr;
    }
    return rb->size - rb->out_ptr + rb->in_ptr;
}

/**
 * @brief Raise high_water to the current fill, mutex already held.
 * @param[in,out] rb  Buffer.
 */
static HOT_PATH_CODE void update_high_water(ring_buffer *rb)
{
    int length = length_locked(rb);

    if (length > rb->high_water)
    {
        rb->high_water = length;
    }
}

void ring_buffer_write(ring_buffer *rb, uint8_t data)
{
    if (lock_take(&rb->mutex))
    {
        if (!rb->full)
        {
            int next = rb->in_ptr + 1;
            if (next == rb->size)
            {
                next = 0;
            }
            rb->data[rb->in_ptr] = data;
            rb->in_ptr = next;
            rb->empty = false;
            rb->full = (next == rb->out_ptr);
            update_high_water(rb);
        }
        lock_give(&rb->mutex);
    }
}

bool HOT_PATH_CODE ring_buffer_write_block(ring_buffer *rb, const uint8_t *data, int n)
{
    bool written = false;

    if (n < 0 || n > rb->size - 1)
    {
        return false;
    }

    if (lock_take(&rb->mutex))
    {
        if (rb->size - length_locked(rb) >= n)
        {
            for (int i = 0; i < n; i++)
            {
                int next = rb->in_ptr + 1;
                if (next == rb->size)
                {
                    next = 0;
                }
                rb->data[rb->in_ptr] = data[i];
                rb->in_ptr = next;
            }
            if (n > 0)
            {
                rb->empty = false;
                rb->full = (rb->in_ptr == rb->out_ptr);
                update_high_water(rb);
            }
            written = true;
        }
        lock_give(&rb->mutex);
    }
    return written;
}

int HOT_PATH_CODE ring_buffer_write_some(ring_buffer *rb, const uint8_t *data, int n)
{
    int count = 0;

    if (n <= 0)
    {
        return 0;
    }
    if (lock_take(&rb->mutex))
    {
        int room = rb->size - length_locked(rb);

        count = (n < room) ? n : room;
        for (int i = 0; i < count; i++)
        {
            int next = rb->in_ptr + 1;
            if (next == rb->size)
            {
                next = 0;
            }
            rb->data[rb->in_ptr] = data[i];
            rb->in_ptr = next;
        }
        if (count > 0)
        {
            rb->empty = false;
            rb->full = (rb->in_ptr == rb->out_ptr);
            update_high_water(rb);
        }
        lock_give(&rb->mutex);
    }
    return count;
}

int HOT_PATH_CODE ring_buffer_read_block(ring_buffer *rb, uint8_t *out, int max)
{
    int count = 0;

    if (max <= 0)
    {
        return 0;
    }
    if (lock_take(&rb->mutex))
    {
        int length = length_locked(rb);

        count = (max < length) ? max : length;
        for (int i = 0; i < count; i++)
        {
            int next = rb->out_ptr + 1;
            if (next == rb->size)
            {
                next = 0;
            }
            out[i] = rb->data[rb->out_ptr];
            rb->out_ptr = next;
        }
        if (count > 0)
        {
            rb->full = false;
            rb->empty = (rb->out_ptr == rb->in_ptr);
        }
        lock_give(&rb->mutex);
    }
    return count;
}

uint8_t ring_buffer_read(ring_buffer *rb)
{
    uint8_t out = 0;

    if (lock_take(&rb->mutex))
    {
        if (!rb->empty)
        {
            int next = rb->out_ptr + 1;
            if (next == rb->size)
            {
                next = 0;
            }
            out = rb->data[rb->out_ptr];
            rb->out_ptr = next;
            rb->full = false;
            rb->empty = (next == rb->in_ptr);
        }
        lock_give(&rb->mutex);
    }
    return out;
}

bool ring_buffer_is_empty(const ring_buffer *rb)
{
    return rb->empty;
}

bool ring_buffer_is_full(const ring_buffer *rb)
{
    return rb->full;
}

int ring_buffer_length(ring_buffer *rb)
{
    int length = 0;

    if (lock_take(&rb->mutex))
    {
        length = length_locked(rb);
        lock_give(&rb->mutex);
    }
    return length;
}

#endif /* not Raspberry Pi Pico */
