/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * OpenSL1 - USB backend (libusb or IOUSBHost): opens the SL1 and streams isochronous audio
 * between the device and per-stream ring buffers.
 */
#ifndef SL1_USB_H
#define SL1_USB_H

#include <stddef.h>
#include <stdint.h>

#include "sl1_proto.h"
#include "sl1_ring.h"

typedef struct sl1_device sl1_device;

typedef struct {
    uint64_t packets;       /* completed iso packets */
    uint64_t frames;        /* frames moved */
    uint64_t packet_errors; /* packets with non-zero status */
    uint64_t xrun;          /* IN: ring full (dropped), OUT: ring empty (silence sent) */
    uint64_t resubmit_fail;
    unsigned min_frames;    /* smallest/largest packet seen, in frames */
    unsigned max_frames;
} sl1_stream_stats;

typedef struct {
    int packets_per_transfer; /* 1 ms each */
    int transfers;            /* in flight per stream */
    int out_transfers;        /* in flight per OUT stream; 0 = same as transfers */
    size_t ring_frames;       /* ring buffer capacity per stream */
} sl1_config;

void sl1_config_default(sl1_config *cfg);

/* Opens the first SL1 found. Error text goes to err. Returns 0 on success. */
int sl1_open(sl1_device **out, const sl1_config *cfg, char *err, size_t errlen);
void sl1_close(sl1_device *dev);

/* Start the streams in mask (SL1_STREAM_BIT(...)). Returns 0 on success. */
int sl1_start(sl1_device *dev, unsigned mask, char *err, size_t errlen);
void sl1_stop(sl1_device *dev);

/* Ring of a stream: IN streams are read by the client, OUT streams written. */
sl1_ring *sl1_stream_ring(sl1_device *dev, sl1_stream_id id);

#ifdef __APPLE__
/*
 * Per-packet hooks for clients that need exact positions (the CoreAudio
 * driver). Set before sl1_start; the rings are then unused. Both run on the
 * USB queue. usb_frame is the 1 ms bus frame the packet is scheduled in.
 */
typedef struct {
    void (*in_packet)(void *ctx, sl1_stream_id id, const int16_t *data, unsigned frames,
                      uint64_t usb_frame);
    /* Write up to max_frames into data; returns the number of frames to send. */
    unsigned (*out_packet)(void *ctx, sl1_stream_id id, int16_t *data, unsigned max_frames,
                           uint64_t usb_frame);
    void *ctx;
} sl1_callbacks;

void sl1_set_callbacks(sl1_device *dev, const sl1_callbacks *cb);

/* Current bus frame and the host time (mach_absolute_time units) it started at. */
int sl1_frame_time(sl1_device *dev, uint64_t *frame, uint64_t *host_time);

/* USB location ID, stable for a given port. */
int sl1_location_id(sl1_device *dev, uint32_t *location);
#endif

void sl1_stream_get_stats(sl1_device *dev, sl1_stream_id id, sl1_stream_stats *out);

#endif
