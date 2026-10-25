/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * OpenSL1 - single-producer/single-consumer ring buffer of interleaved
 * S16 frames. One side is the USB event thread, the other the client.
 */
#ifndef SL1_RING_H
#define SL1_RING_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int16_t *data;
    size_t capacity; /* in frames, power of two */
    int channels;
    _Atomic size_t head; /* write position (frames, monotonic) */
    _Atomic size_t tail; /* read position (frames, monotonic) */
} sl1_ring;

/* capacity_frames is rounded up to a power of two. Returns 0 on success. */
int sl1_ring_init(sl1_ring *r, size_t capacity_frames, int channels);
void sl1_ring_free(sl1_ring *r);

size_t sl1_ring_available(sl1_ring *r); /* frames ready to read */
size_t sl1_ring_space(sl1_ring *r);     /* frames that can be written */

/* Both return the number of frames actually transferred. */
size_t sl1_ring_write(sl1_ring *r, const int16_t *frames, size_t count);
size_t sl1_ring_read(sl1_ring *r, int16_t *frames, size_t count);

#endif
