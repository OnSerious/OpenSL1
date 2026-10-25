/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sl1_usb.h"

#include <libusb.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sl1_pkt.h"

typedef struct stream stream;

typedef struct {
    stream *owner;
    struct libusb_transfer *xfer;
    unsigned char *buf;
} xfer_slot;

struct stream {
    sl1_device *dev;
    sl1_stream_id id;
    const sl1_stream_info *info;
    int active;
    sl1_ring ring;
    sl1_pkt_clock clock; /* OUT only */
    xfer_slot *slots;
    int nslots;
    _Atomic int in_flight;
    pthread_mutex_t stats_lock;
    sl1_stream_stats stats;
    int16_t *scratch; /* OUT: frames pulled from the ring for one packet */
};

struct sl1_device {
    libusb_context *ctx;
    libusb_device_handle *h;
    sl1_config cfg;
    stream streams[SL1_STREAM_COUNT];
    int claimed[SL1_NUM_INTERFACES];
    _Atomic int running;
    pthread_t event_thread;
    int event_thread_started;
};

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    if (!err || !errlen)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

void sl1_config_default(sl1_config *cfg)
{
    cfg->packets_per_transfer = 8;
    cfg->transfers = 4;
    cfg->out_transfers = 0;
    cfg->ring_frames = SL1_RATE; /* 1 s */
}

static void stats_packet(stream *s, unsigned frames, int status_ok)
{
    pthread_mutex_lock(&s->stats_lock);
    s->stats.packets++;
    s->stats.frames += frames;
    if (!status_ok)
        s->stats.packet_errors++;
    if (status_ok) {
        if (s->stats.min_frames == 0 || frames < s->stats.min_frames)
            s->stats.min_frames = frames;
        if (frames > s->stats.max_frames)
            s->stats.max_frames = frames;
    }
    pthread_mutex_unlock(&s->stats_lock);
}

static void stats_xrun(stream *s)
{
    pthread_mutex_lock(&s->stats_lock);
    s->stats.xrun++;
    pthread_mutex_unlock(&s->stats_lock);
}

/* Fill an OUT transfer: packet lengths from the 44.1 kHz clock, audio from the ring. */
static void fill_out(stream *s, struct libusb_transfer *t)
{
    int ch = s->info->channels;
    size_t frame_bytes = (size_t)ch * SL1_BYTES_PER_SAMPLE;
    unsigned char *p = t->buffer;
    int total = 0;
    for (int i = 0; i < t->num_iso_packets; i++) {
        unsigned frames = sl1_pkt_clock_next(&s->clock);
        size_t got = sl1_ring_read(&s->ring, s->scratch, frames);
        if (got < frames) {
            memset(s->scratch + got * (size_t)ch, 0, (frames - got) * frame_bytes);
            stats_xrun(s);
        }
        memcpy(p, s->scratch, frames * frame_bytes);
        t->iso_packet_desc[i].length = (unsigned)(frames * frame_bytes);
        p += frames * frame_bytes;
        total += (int)(frames * frame_bytes);
    }
    t->length = total;
}

static void LIBUSB_CALL xfer_cb(struct libusb_transfer *t)
{
    xfer_slot *slot = t->user_data;
    stream *s = slot->owner;
    sl1_device *dev = s->dev;
    size_t frame_bytes = (size_t)s->info->channels * SL1_BYTES_PER_SAMPLE;

    if (t->status == LIBUSB_TRANSFER_COMPLETED) {
        for (int i = 0; i < t->num_iso_packets; i++) {
            struct libusb_iso_packet_descriptor *d = &t->iso_packet_desc[i];
            int ok = d->status == LIBUSB_TRANSFER_COMPLETED;
            if (s->info->is_input) {
                unsigned bytes = d->actual_length > (unsigned)s->info->max_packet
                                     ? (unsigned)s->info->max_packet
                                     : d->actual_length;
                unsigned frames = ok ? bytes / frame_bytes : 0;
                if (frames) {
                    const int16_t *src =
                        (const int16_t *)libusb_get_iso_packet_buffer_simple(t, (unsigned)i);
                    if (sl1_ring_write(&s->ring, src, frames) < frames)
                        stats_xrun(s);
                }
                stats_packet(s, frames, ok);
            } else {
                stats_packet(s, (unsigned)(d->length / frame_bytes), ok);
            }
        }
    }

    if (atomic_load(&dev->running) && t->status != LIBUSB_TRANSFER_CANCELLED &&
        t->status != LIBUSB_TRANSFER_NO_DEVICE) {
        if (!s->info->is_input)
            fill_out(s, t);
        if (libusb_submit_transfer(t) == 0)
            return;
        pthread_mutex_lock(&s->stats_lock);
        s->stats.resubmit_fail++;
        pthread_mutex_unlock(&s->stats_lock);
    }
    atomic_fetch_sub(&s->in_flight, 1);
}

static void *event_loop(void *arg)
{
    sl1_device *dev = arg;
    struct timeval tv = { 0, 100000 };
    while (atomic_load(&dev->running))
        libusb_handle_events_timeout_completed(dev->ctx, &tv, NULL);
    return NULL;
}

int sl1_open(sl1_device **out, const sl1_config *cfg, char *err, size_t errlen)
{
    sl1_device *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        set_err(err, errlen, "out of memory");
        return -1;
    }
    if (cfg)
        dev->cfg = *cfg;
    else
        sl1_config_default(&dev->cfg);

    int rc = libusb_init_context(&dev->ctx, NULL, 0);
    if (rc) {
        set_err(err, errlen, "libusb_init: %s", libusb_strerror(rc));
        free(dev);
        return -1;
    }
    dev->h = libusb_open_device_with_vid_pid(dev->ctx, SL1_VID, SL1_PID);
    if (!dev->h) {
        set_err(err, errlen,
                "SL1 (%04x:%04x) not found or no access (on Linux install udev/50-opensl1.rules)",
                SL1_VID, SL1_PID);
        libusb_exit(dev->ctx);
        free(dev);
        return -1;
    }
    /* Kernel drivers (Linux: snd-usb-audio) are detached on claim and reattached on release. */
    libusb_set_auto_detach_kernel_driver(dev->h, 1);
    for (int i = 0; i < SL1_NUM_INTERFACES; i++) {
        rc = libusb_claim_interface(dev->h, i);
        if (rc) {
            set_err(err, errlen, "claim interface %d: %s", i, libusb_strerror(rc));
            sl1_close(dev);
            return -1;
        }
        dev->claimed[i] = 1;
    }
    for (int i = 0; i < SL1_STREAM_COUNT; i++) {
        stream *s = &dev->streams[i];
        s->dev = dev;
        s->id = (sl1_stream_id)i;
        s->info = &SL1_STREAMS[i];
        pthread_mutex_init(&s->stats_lock, NULL);
        if (sl1_ring_init(&s->ring, dev->cfg.ring_frames, s->info->channels)) {
            set_err(err, errlen, "out of memory");
            sl1_close(dev);
            return -1;
        }
    }
    *out = dev;
    return 0;
}

static int stream_start(stream *s, char *err, size_t errlen)
{
    sl1_device *dev = s->dev;
    const sl1_config *cfg = &dev->cfg;
    int rc = libusb_set_interface_alt_setting(dev->h, s->info->interface, SL1_ALT_ACTIVE);
    if (rc) {
        set_err(err, errlen, "%s: set alt %d on interface %d: %s", s->info->name,
                SL1_ALT_ACTIVE, s->info->interface, libusb_strerror(rc));
        return -1;
    }
    sl1_pkt_clock_init(&s->clock, SL1_RATE, 1000);
    s->scratch = calloc((size_t)s->info->max_packet, 1);
    s->nslots = !s->info->is_input && cfg->out_transfers > 0 ? cfg->out_transfers : cfg->transfers;
    s->slots = calloc((size_t)s->nslots, sizeof(xfer_slot));
    if (!s->scratch || !s->slots) {
        set_err(err, errlen, "out of memory");
        return -1;
    }
    memset(&s->stats, 0, sizeof(s->stats));
    s->active = 1;
    size_t buflen = (size_t)s->info->max_packet * (size_t)cfg->packets_per_transfer;
    for (int i = 0; i < s->nslots; i++) {
        xfer_slot *slot = &s->slots[i];
        slot->owner = s;
        slot->buf = calloc(buflen, 1);
        slot->xfer = libusb_alloc_transfer(cfg->packets_per_transfer);
        if (!slot->buf || !slot->xfer) {
            set_err(err, errlen, "out of memory");
            return -1;
        }
        libusb_fill_iso_transfer(slot->xfer, dev->h, s->info->endpoint, slot->buf, (int)buflen,
                                 cfg->packets_per_transfer, xfer_cb, slot, 1000);
        if (s->info->is_input)
            libusb_set_iso_packet_lengths(slot->xfer, (unsigned)s->info->max_packet);
        else
            fill_out(s, slot->xfer);
        rc = libusb_submit_transfer(slot->xfer);
        if (rc) {
            set_err(err, errlen, "%s: submit: %s", s->info->name, libusb_strerror(rc));
            return -1;
        }
        atomic_fetch_add(&s->in_flight, 1);
    }
    return 0;
}

int sl1_start(sl1_device *dev, unsigned mask, char *err, size_t errlen)
{
    atomic_store(&dev->running, 1);
    if (pthread_create(&dev->event_thread, NULL, event_loop, dev)) {
        set_err(err, errlen, "cannot start event thread");
        atomic_store(&dev->running, 0);
        return -1;
    }
    dev->event_thread_started = 1;
    for (int i = 0; i < SL1_STREAM_COUNT; i++) {
        if (!(mask & SL1_STREAM_BIT(i)))
            continue;
        if (stream_start(&dev->streams[i], err, errlen)) {
            sl1_stop(dev);
            return -1;
        }
    }
    return 0;
}

void sl1_stop(sl1_device *dev)
{
    atomic_store(&dev->running, 0);
    for (int i = 0; i < SL1_STREAM_COUNT; i++) {
        stream *s = &dev->streams[i];
        if (!s->active)
            continue;
        for (int k = 0; k < s->nslots; k++)
            if (s->slots[k].xfer)
                libusb_cancel_transfer(s->slots[k].xfer);
    }
    /* Let the event thread finish, then drain cancellations here. */
    if (dev->event_thread_started) {
        pthread_join(dev->event_thread, NULL);
        dev->event_thread_started = 0;
    }
    struct timeval tv = { 0, 50000 };
    for (int tries = 0; tries < 40; tries++) {
        int busy = 0;
        for (int i = 0; i < SL1_STREAM_COUNT; i++)
            busy += atomic_load(&dev->streams[i].in_flight);
        if (!busy)
            break;
        libusb_handle_events_timeout_completed(dev->ctx, &tv, NULL);
    }
    for (int i = 0; i < SL1_STREAM_COUNT; i++) {
        stream *s = &dev->streams[i];
        if (!s->active)
            continue;
        for (int k = 0; k < s->nslots; k++) {
            libusb_free_transfer(s->slots[k].xfer);
            free(s->slots[k].buf);
        }
        free(s->slots);
        free(s->scratch);
        s->slots = NULL;
        s->scratch = NULL;
        libusb_set_interface_alt_setting(dev->h, s->info->interface, SL1_ALT_IDLE);
        s->active = 0;
    }
}

void sl1_close(sl1_device *dev)
{
    if (!dev)
        return;
    sl1_stop(dev);
    for (int i = 0; i < SL1_NUM_INTERFACES; i++)
        if (dev->claimed[i])
            libusb_release_interface(dev->h, i);
    for (int i = 0; i < SL1_STREAM_COUNT; i++) {
        if (dev->streams[i].ring.data)
            sl1_ring_free(&dev->streams[i].ring);
        if (dev->streams[i].dev)
            pthread_mutex_destroy(&dev->streams[i].stats_lock);
    }
    if (dev->h)
        libusb_close(dev->h);
    if (dev->ctx)
        libusb_exit(dev->ctx);
    free(dev);
}

sl1_ring *sl1_stream_ring(sl1_device *dev, sl1_stream_id id)
{
    return &dev->streams[id].ring;
}

void sl1_stream_get_stats(sl1_device *dev, sl1_stream_id id, sl1_stream_stats *out)
{
    stream *s = &dev->streams[id];
    pthread_mutex_lock(&s->stats_lock);
    *out = s->stats;
    pthread_mutex_unlock(&s->stats_lock);
}
