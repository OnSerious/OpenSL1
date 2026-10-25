/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sl1_ring.h"

#include <stdlib.h>
#include <string.h>

int sl1_ring_init(sl1_ring *r, size_t capacity_frames, int channels)
{
    size_t cap = 1;
    while (cap < capacity_frames)
        cap <<= 1;
    r->data = calloc(cap * (size_t)channels, sizeof(int16_t));
    if (!r->data)
        return -1;
    r->capacity = cap;
    r->channels = channels;
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    return 0;
}

void sl1_ring_free(sl1_ring *r)
{
    free(r->data);
    r->data = NULL;
}

size_t sl1_ring_available(sl1_ring *r)
{
    size_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    return head - tail;
}

size_t sl1_ring_space(sl1_ring *r)
{
    return r->capacity - sl1_ring_available(r);
}

/* Copy count frames between linear memory and the ring at frame position pos. */
static void ring_copy(sl1_ring *r, size_t pos, int16_t *linear, size_t count, int to_ring)
{
    size_t idx = pos & (r->capacity - 1);
    size_t first = r->capacity - idx;
    if (first > count)
        first = count;
    size_t ch = (size_t)r->channels;
    int16_t *ring_a = r->data + idx * ch;
    int16_t *ring_b = r->data;
    if (to_ring) {
        memcpy(ring_a, linear, first * ch * sizeof(int16_t));
        memcpy(ring_b, linear + first * ch, (count - first) * ch * sizeof(int16_t));
    } else {
        memcpy(linear, ring_a, first * ch * sizeof(int16_t));
        memcpy(linear + first * ch, ring_b, (count - first) * ch * sizeof(int16_t));
    }
}

size_t sl1_ring_write(sl1_ring *r, const int16_t *frames, size_t count)
{
    size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    size_t space = r->capacity - (head - tail);
    if (count > space)
        count = space;
    ring_copy(r, head, (int16_t *)frames, count, 1);
    atomic_store_explicit(&r->head, head + count, memory_order_release);
    return count;
}

size_t sl1_ring_read(sl1_ring *r, int16_t *frames, size_t count)
{
    size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    size_t avail = head - tail;
    if (count > avail)
        count = avail;
    ring_copy(r, tail, frames, count, 0);
    atomic_store_explicit(&r->tail, tail + count, memory_order_release);
    return count;
}
