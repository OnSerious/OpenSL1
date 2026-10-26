/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * OpenSL1 - CoreAudio AudioServerPlugIn for the Rane SL1.
 *
 * Publishes one device "Rane SL1" while the SL1 is plugged in:
 *   input  4 ch: Input 1 L/R, Input 2 L/R
 *   output 4 ch: Line 1 L/R, Line 2 L/R
 * at 44.1 kHz, float32. USB goes through the IOUSBHost backend (sl1_usb_mac.m).
 *
 * Clock: the SL1 sends 44/45 frames per 1 ms bus frame. The device sample
 * counter is the number of frames received on Input 1; zero timestamps map it
 * to host time via the bus frame clock. Output packet sizes follow the same
 * counter, so the HAL and the device never drift apart.
 *
 * Written from Apple's AudioServerPlugIn.h; no code from other drivers.
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <math.h>
#include <notify.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/log.h>
#include <unistd.h>

#include "../src/sl1_usb.h"

/* Unified log (log stream --predicate 'subsystem == "org.opensl1.driver"'), and stderr for the test harness. */
#define LOG(fmt, ...)                                                                              \
    do {                                                                                           \
        os_log(opensl1_log(), "OpenSL1: " fmt, ##__VA_ARGS__);                                     \
        fprintf(stderr, "OpenSL1: " fmt "\n", ##__VA_ARGS__);                                      \
    } while (0)

/* A message plus one runtime string (strings are redacted in the log unless marked public). */
#define LOG_STR(what, str)                                                                         \
    do {                                                                                           \
        os_log(opensl1_log(), "OpenSL1: " what ": %{public}s", (str));                            \
        fprintf(stderr, "OpenSL1: " what ": %s\n", (str));                                         \
    } while (0)

static os_log_t opensl1_log(void)
{
    static os_log_t log;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        log = os_log_create("org.opensl1.driver", "driver");
    });
    return log;
}

/*
 * While the SL1 hangs, the watchdog retries every few hundred ms and would log
 * the same failure several times a second. A gate lets a message through once
 * per LOG_GATE_NS and counts the repeats in between. Callers hold g.lock.
 */
#define LOG_GATE_NS (10ull * NSEC_PER_SEC)

typedef struct {
    char last[256];
    uint64_t last_ns;
    unsigned suppressed;
} log_gate;

static void gate_flush(log_gate *gt)
{
    if (gt->suppressed)
        LOG("%u repeated messages suppressed", gt->suppressed);
    gt->suppressed = 0;
    gt->last[0] = 0;
}

static int gate_pass(log_gate *gt, const char *text)
{
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    if (gt->last[0] && strcmp(text, gt->last) == 0 && now - gt->last_ns < LOG_GATE_NS) {
        gt->suppressed++;
        return 0;
    }
    gate_flush(gt);
    snprintf(gt->last, sizeof(gt->last), "%s", text);
    gt->last_ns = now;
    return 1;
}

static log_gate gate_open, gate_start, gate_watchdog;

enum {
    OBJ_PLUGIN = kAudioObjectPlugInObject,
    OBJ_DEVICE = 2,
    OBJ_STREAM_IN = 3,
    OBJ_STREAM_OUT = 4,
};

#define CHANNELS 4
#define RING_FRAMES 16384 /* power of two; also the zero timestamp period */
#define RING_MASK (RING_FRAMES - 1)

/*
 * USB scheduling, 2 packets (2 ms) per request. Input keeps 12 requests in
 * flight (24 ms of slack for a late USB queue, no extra latency). Output keeps
 * 6 in flight, so USB takes output up to 12 ms ahead of time.
 * Safety offsets = what USB holds + measured completion jitter + margin; check
 * the "headroom" log line after changing any of these.
 */
#define USB_PACKETS 2
#define USB_TRANSFERS_IN 12
#define USB_TRANSFERS_OUT 6
#define SAFETY_IN 397  /* 9 ms: with 6 ms coreaudiod left only 4 frames of headroom (Mixxx, 2026-10-25) */
/*
 * Hardware loop Line 1 -> Input 1 (USB, converters, input stage) measured with
 * tests/ca_loop: 128 frames, identical in every run. How it splits between
 * input and output is unknown; reported as half each.
 */
#define LATENCY_IN 64
#define LATENCY_OUT 64
#define SAFETY_OUT 529 /* 12 ms: with 16 ms the real HAL left 390 frames (8.8 ms) unused (Mixxx, 2026-10-25) */

/* Watchdog: restart USB if Input 1 delivers nothing for this long while IO runs. */
#define WATCHDOG_MS 100
#define WATCHDOG_STALL_TICKS 3

#define MODEL_UID "OpenSL1:13e5:0001"

static const char *const CHANNEL_NAMES_IN[CHANNELS] = { "Input 1 L", "Input 1 R", "Input 2 L",
                                                        "Input 2 R" };
static const char *const CHANNEL_NAMES_OUT[CHANNELS] = { "Line 1 L", "Line 1 R", "Line 2 L",
                                                         "Line 2 R" };

/* Clock published by the USB queue, read by the IO thread (seqlock). */
typedef struct {
    _Atomic uint32_t seq;
    double sample;   /* device sample count ... */
    double host;     /* ... at this host time */
    double per_tick; /* samples per host tick */
} clock_pub;

static struct {
    AudioServerPlugInHostRef host;
    pthread_mutex_t lock;
    dispatch_queue_t notify_queue;
    IONotificationPortRef notify_port;
    io_iterator_t added_iter, removed_iter;

    int attached; /* the SL1 is plugged in */
    _Atomic int present; /* the device is published (always, see drv_Initialize) */
    uint32_t location;
    _Atomic UInt32 io_clients; /* changed under lock, read lock-free by property calls */
    sl1_device *usb;
    _Atomic UInt32 stream_active[2];

    /* Shared rings, indexed by device sample time. */
    int16_t in_ring[RING_FRAMES * CHANNELS];
    int16_t out_ring[RING_FRAMES * CHANNELS];

    /* USB queue only. */
    int a_started, b_started;
    uint64_t f0;         /* bus frame of the first Input 1 packet */
    uint64_t a_count;    /* frames received on Input 1 = device sample counter */
    uint64_t a_base;     /* a_count at f0 (non-zero after a watchdog restart) */
    uint64_t a_end;      /* bus frame after the last Input 1 packet */
    uint64_t b_count;
    /* Input meter (USB queue only), logged and reset every METER_PACKETS of Input 1. */
    struct {
        double sum2[2], cross; /* per channel sum of squares, sum of L*R */
        int peak[2];
        uint64_t frames, clipped, packets;
        uint64_t gap_frames;   /* silence inserted for missed bus frames */
        uint64_t empty;        /* packets that arrived without data */
        uint64_t resyncs;      /* Input 2 only: counter re-aligned to Input 1 */
    } meter[2];
    double rate_f;       /* frames per bus frame, ~44.1 */
    uint64_t ref_frame0; /* first bus frame / host time pair */
    uint64_t ref_host0;
    double ticks_per_frame; /* nominal 1 ms */
    double env[32];         /* recent offsets of (frame, time) pairs from the model */
    unsigned env_n;
    double offset;          /* smoothed lower envelope of env */
    unsigned packets;
    int out_started[2];
    uint64_t out_next[2];
    uint64_t out_frames[2], out_audible[2]; /* diagnostics, logged on stop */

    clock_pub clock;
    _Atomic int clock_valid; /* set by the first packet after StartIO, cleared by StopIO */
    _Atomic UInt64 seed;
    _Atomic uint64_t in_packets; /* progress counter for the watchdog */

    /*
     * Headroom, logged on stop: how early input data was there before the HAL
     * read it, and how far ahead the HAL wrote output before USB took it.
     * Below 0 means the HAL read stale input or USB sent silence.
     */
    _Atomic uint64_t in_written;  /* Input 1 frames in the ring */
    _Atomic uint64_t b_written;   /* Input 2 frames in the ring (own freshness, it can lag or stop) */
    _Atomic uint64_t progress[4]; /* packets per stream (A out, A in, B out, B in) for the watchdog */
    _Atomic uint64_t out_taken;   /* Line 1 frames taken by USB */
    _Atomic int64_t in_margin_min, out_margin_min;
    _Atomic uint64_t in_late, out_late;
    _Atomic int64_t in_late_last, out_late_last; /* sample time of the last late access */
    int64_t io_start_sample;

    /* Watchdog (notify queue). */
    dispatch_source_t watchdog;
    uint64_t wd_last[4];
    int wd_stall;
    unsigned wd_ticks;
    unsigned restarts;
    /* Set for a watchdog restart: continue the old timeline instead of starting at 0. */
    int resume;
    int free_running; /* IO runs on the nominal clock, without the SL1 */
    double resume_sample, resume_host, resume_per_tick;
    /* Test hook (hal_harness only): OPENSL1_TEST_STALL=<n> ignores Input 1 for 500 packets from packet n. */
    uint64_t test_stall_at;
    /* Test hook (hal_harness only): OPENSL1_TEST_STALL_B=<n> does the same for Input 2 alone. */
    uint64_t test_stall_b_at;
    /* Test hook (hal_harness only): OPENSL1_TEST_OPEN_FAIL=<n> makes the first n opens fail. */
    unsigned test_open_fail;
    int power_token;
} g = { .lock = PTHREAD_MUTEX_INITIALIZER, .stream_active = { 1, 1 }, .seed = 1 };

/* ---- clock -------------------------------------------------------------- */

static void clock_publish(double sample, double host, double per_tick)
{
    uint32_t s = atomic_load_explicit(&g.clock.seq, memory_order_relaxed);
    atomic_store_explicit(&g.clock.seq, s + 1, memory_order_release);
    atomic_thread_fence(memory_order_release);
    g.clock.sample = sample;
    g.clock.host = host;
    g.clock.per_tick = per_tick;
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&g.clock.seq, s + 2, memory_order_release);
}

static void clock_read(double *sample, double *host, double *per_tick)
{
    /* Bounded: the IO thread must never spin forever on a stuck writer. */
    for (int tries = 0;; tries++) {
        uint32_t s1 = atomic_load_explicit(&g.clock.seq, memory_order_acquire);
        if ((s1 & 1) && tries < 1000)
            continue;
        *sample = g.clock.sample;
        *host = g.clock.host;
        *per_tick = g.clock.per_tick;
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&g.clock.seq, memory_order_acquire) == s1)
            return;
    }
}

/* Estimated device sample position at the start of a bus frame. */
static double sample_at_frame(uint64_t frame)
{
    return (double)g.a_count + ((double)frame - (double)g.a_end) * g.rate_f;
}

/*
 * Bus frame -> host time. frameNumberWithTime: returns the time of the query
 * but a whole frame number, so each pair is late by 0..1 ms (measured: up to
 * 0.6 ms of scatter, no trend). Slope stays at the nominal 1 ms per frame; the
 * offset follows the lower envelope of recent pairs (= real frame starts),
 * smoothed, so the zero timestamps neither jitter nor drift away.
 */
static void refresh_frame_ref(void)
{
    uint64_t f, t;
    if (sl1_frame_time(g.usb, &f, &t))
        return;
    if (!g.ref_frame0) {
        g.ref_frame0 = f;
        g.ref_host0 = t;
        g.env_n = 0;
        g.offset = 0;
    }
    double r = (double)t - ((double)g.ref_host0 + ((double)f - (double)g.ref_frame0) * g.ticks_per_frame);
    g.env[g.env_n++ % 32] = r;
    unsigned n = g.env_n < 32 ? g.env_n : 32;
    double lo = g.env[0];
    for (unsigned i = 1; i < n; i++)
        if (g.env[i] < lo)
            lo = g.env[i];
    g.offset = g.env_n == 1 ? lo : g.offset + 0.2 * (lo - g.offset);
}

static double host_at_frame(uint64_t frame)
{
    return (double)g.ref_host0 + g.offset + ((double)frame - (double)g.ref_frame0) * g.ticks_per_frame;
}

/* ---- USB callbacks (USB queue) ------------------------------------------ */

static void ring_put(const int16_t *src, unsigned frames, uint64_t pos, int ch0)
{
    for (unsigned i = 0; i < frames; i++) {
        int16_t *dst = &g.in_ring[((pos + i) & RING_MASK) * CHANNELS + (unsigned)ch0];
        dst[0] = src ? src[2 * i] : 0;
        dst[1] = src ? src[2 * i + 1] : 0;
    }
}

/* Silences ring positions [from, to) of one input pair (at most one ring lap). */
static void ring_silence(uint64_t from, uint64_t to, int ch0)
{
    if (to <= from)
        return;
    uint64_t n = to - from > RING_FRAMES ? RING_FRAMES : to - from;
    ring_put(NULL, (unsigned)n, to - n, ch0);
}

/* Frames a packet in this bus frame should have carried, from the measured rate. */
static unsigned expected_frames(uint64_t pos, uint64_t usb_frame)
{
    double expect = sample_at_frame(usb_frame + 1) - (double)pos;
    return expect < 40 ? 40 : expect > 48 ? 48 : (unsigned)lround(expect);
}

/* Input meter: helps judge the timecode signal (level, clipping, L/R phase) and gaps. */
#define METER_PACKETS 30000 /* 30 s */
#define CLIP_LEVEL 32700

static void meter_add(int k, const int16_t *data, unsigned frames)
{
    if (!data)
        return;
    for (unsigned i = 0; i < frames; i++) {
        int l = data[2 * i], r = data[2 * i + 1];
        g.meter[k].sum2[0] += (double)l * l;
        g.meter[k].sum2[1] += (double)r * r;
        g.meter[k].cross += (double)l * r;
        if (abs(l) > g.meter[k].peak[0])
            g.meter[k].peak[0] = abs(l);
        if (abs(r) > g.meter[k].peak[1])
            g.meter[k].peak[1] = abs(r);
        if (abs(l) >= CLIP_LEVEL || abs(r) >= CLIP_LEVEL)
            g.meter[k].clipped++;
    }
    g.meter[k].frames += frames;
}

static double dbfs(double v)
{
    return v > 0 ? 20.0 * log10(v / 32768.0) : -120.0;
}

static void meter_log_and_reset(void)
{
    for (int k = 0; k < 2; k++) {
        double n = g.meter[k].frames ? (double)g.meter[k].frames : 1.0;
        double l2 = g.meter[k].sum2[0], r2 = g.meter[k].sum2[1];
        /* Timecode is two tones 90 degrees apart, so the correlation should be near 0. */
        double corr = l2 > 0 && r2 > 0 ? g.meter[k].cross / sqrt(l2 * r2) : 0.0;
        os_log(opensl1_log(),
               "OpenSL1: input %d: rms L %.1f R %.1f dBFS, peak L %.1f R %.1f dBFS, %llu clipped, "
               "L/R corr %.2f, %llu gap frames, %llu empty packets, %llu resyncs",
               k + 1, dbfs(sqrt(l2 / n)), dbfs(sqrt(r2 / n)), dbfs(g.meter[k].peak[0]),
               dbfs(g.meter[k].peak[1]), g.meter[k].clipped, corr, g.meter[k].gap_frames,
               g.meter[k].empty, g.meter[k].resyncs);
    }
    memset(g.meter, 0, sizeof(g.meter));
}

static void on_in(void *ctx, sl1_stream_id id, const int16_t *data, unsigned frames,
                  uint64_t usb_frame)
{
    (void)ctx;
    if (id == SL1_STREAM_A_IN) {
        if (g.test_stall_at) {
            static uint64_t seen;
            if (++seen >= g.test_stall_at && seen < g.test_stall_at + 500)
                return;
        }
        if (!g.a_started) {
            g.a_started = 1;
            g.f0 = usb_frame;
            g.a_end = usb_frame;
            g.rate_f = SL1_RATE / 1000.0;
            refresh_frame_ref();
            g.a_count = 0;
            if (g.resume) {
                /* Same timeline as before the restart, so the HAL sees no jump. */
                double s0 = g.resume_sample + (host_at_frame(usb_frame) - g.resume_host) * g.resume_per_tick;
                g.a_count = s0 > 0 ? (uint64_t)llround(s0) : 0;
                g.rate_f = g.resume_per_tick * g.ticks_per_frame;
                g.resume = 0;
                /* Positions skipped during the stall hold data from an older ring lap. */
                ring_silence(atomic_load(&g.in_written), g.a_count, 0);
            }
            g.a_base = g.a_count;
        }
        if (usb_frame < g.a_end)
            return; /* duplicate or out of order: the counter only moves forward */
        /*
         * Bus frames we never got a packet for (late resubmit) and failed
         * packets still took their share of time: fill with silence so the
         * sample counter keeps matching the clock.
         */
        if (usb_frame > g.a_end) {
            int64_t gap = llround((double)(usb_frame - g.a_end) * g.rate_f);
            if (gap > 0) {
                g.meter[0].gap_frames += (uint64_t)gap;
                ring_put(NULL, gap > RING_FRAMES ? RING_FRAMES : (unsigned)gap, g.a_count, 0);
                g.a_count += (uint64_t)gap;
            }
        }
        if (!frames) {
            frames = expected_frames(g.a_count, usb_frame);
            data = NULL;
            g.meter[0].empty++;
        }
        meter_add(0, data, frames);
        ring_put(data, frames, g.a_count, 0);
        g.a_count += frames;
        atomic_store_explicit(&g.in_written, g.a_count, memory_order_release);
        g.a_end = usb_frame + 1;
        if (g.a_end - g.f0 > 1000)
            g.rate_f = (double)(g.a_count - g.a_base) / (double)(g.a_end - g.f0);
        if ((++g.packets & 63) == 0)
            refresh_frame_ref();
        clock_publish((double)g.a_count, host_at_frame(g.a_end), g.rate_f / g.ticks_per_frame);
        atomic_store(&g.clock_valid, 1);
        atomic_fetch_add(&g.in_packets, 1);
        atomic_fetch_add_explicit(&g.progress[SL1_STREAM_A_IN], 1, memory_order_relaxed);
        if (++g.meter[0].packets >= METER_PACKETS)
            meter_log_and_reset();
    } else if (id == SL1_STREAM_B_IN) {
        if (g.test_stall_b_at) {
            static uint64_t seen_b;
            if (++seen_b >= g.test_stall_b_at && seen_b < g.test_stall_b_at + 500)
                return;
        }
        atomic_fetch_add_explicit(&g.progress[SL1_STREAM_B_IN], 1, memory_order_relaxed);
        if (!g.a_started)
            return;
        /* Input 2 follows Input 1's counter; resync if it ever drifts away. */
        int64_t expect = llround(sample_at_frame(usb_frame));
        if (!g.b_started || llabs(expect - (int64_t)g.b_count) > 64) {
            if (g.b_started)
                g.meter[1].resyncs++;
            g.b_started = 1;
            g.b_count = (uint64_t)(expect < 0 ? 0 : expect);
            /* A forward jump skips positions that still hold an older ring lap. */
            ring_silence(atomic_load(&g.b_written), g.b_count, 2);
        }
        if (!frames) {
            frames = expected_frames(g.b_count, usb_frame);
            data = NULL;
            g.meter[1].empty++;
        }
        meter_add(1, data, frames);
        ring_put(data, frames, g.b_count, 2);
        g.b_count += frames;
        atomic_store_explicit(&g.b_written, g.b_count, memory_order_release);
    }
}

static unsigned on_out(void *ctx, sl1_stream_id id, int16_t *data, unsigned max_frames,
                       uint64_t usb_frame)
{
    (void)ctx;
    int k = id == SL1_STREAM_A_OUT ? 0 : 1;
    unsigned ch0 = (unsigned)k * 2;
    atomic_fetch_add_explicit(&g.progress[id], 1, memory_order_relaxed);
    if (!g.a_started) {
        memset(data, 0, 44 * 4);
        return 44;
    }
    int64_t start = llround(sample_at_frame(usb_frame));
    int64_t end = llround(sample_at_frame(usb_frame + 1));
    if (!g.out_started[k] || llabs(start - (int64_t)g.out_next[k]) > 64) {
        g.out_started[k] = 1;
        g.out_next[k] = (uint64_t)(start < 0 ? 0 : start);
    }
    int64_t n = end - (int64_t)g.out_next[k];
    if (n < 0)
        n = 0;
    if (n > (int64_t)max_frames)
        n = max_frames;
    for (int64_t i = 0; i < n; i++) {
        int16_t *src = &g.out_ring[((g.out_next[k] + (uint64_t)i) & RING_MASK) * CHANNELS + ch0];
        data[2 * i] = src[0];
        data[2 * i + 1] = src[1];
        if (src[0] || src[1])
            g.out_audible[k]++;
        src[0] = src[1] = 0; /* never replay stale audio */
    }
    g.out_frames[k] += (uint64_t)n;
    g.out_next[k] += (uint64_t)n;
    if (k == 0)
        atomic_store_explicit(&g.out_taken, g.out_next[0], memory_order_release);
    return (unsigned)n;
}

/* ---- device presence (IOKit notifications) -------------------------------- */

static void notify_changed(AudioObjectID obj, AudioObjectPropertySelector sel)
{
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    if (g.host)
        g.host->PropertiesChanged(g.host, obj, 1, &a);
}

/* Statistics of the current USB session plus headroom since StartIO (lock held). */
static void log_stats_locked(int periodic)
{
    if (!g.usb)
        return;
    sl1_stream_stats st[SL1_STREAM_COUNT];
    for (int i = 0; i < SL1_STREAM_COUNT; i++)
        sl1_stream_get_stats(g.usb, (sl1_stream_id)i, &st[i]);
#define STATS_FMT                                                                                  \
    ": in A %llu pkts %llu err, in B %llu pkts %llu err, out A %llu frames (%llu audible) %llu err, "  \
    "out B %llu frames (%llu audible) %llu err, rate %.5f frames/ms, %u restarts"
#define STATS_ARGS                                                                                 \
    st[SL1_STREAM_A_IN].packets, st[SL1_STREAM_A_IN].packet_errors, st[SL1_STREAM_B_IN].packets,   \
        st[SL1_STREAM_B_IN].packet_errors, g.out_frames[0], g.out_audible[0],                      \
        st[SL1_STREAM_A_OUT].packet_errors, g.out_frames[1], g.out_audible[1],                     \
        st[SL1_STREAM_B_OUT].packet_errors, g.rate_f, g.restarts
    /* Literal formats only: os_log would redact a runtime string. */
    if (periodic)
        LOG("stats (periodic)" STATS_FMT, STATS_ARGS);
    else
        LOG("stats (stop)" STATS_FMT, STATS_ARGS);
#undef STATS_FMT
#undef STATS_ARGS
    LOG("headroom: input min %lld frames (%llu late reads, last at %.2f s), output min %lld frames "
        "(%llu late writes, last at %.2f s)",
        (long long)atomic_load(&g.in_margin_min), atomic_load(&g.in_late),
        atomic_load(&g.in_late) ? (double)(atomic_load(&g.in_late_last) - g.io_start_sample) / SL1_RATE : 0.0,
        (long long)atomic_load(&g.out_margin_min), atomic_load(&g.out_late),
        atomic_load(&g.out_late) ? (double)(atomic_load(&g.out_late_last) - g.io_start_sample) / SL1_RATE : 0.0);
}

static void usb_close_locked(void)
{
    if (!g.usb)
        return;
    log_stats_locked(0);
    sl1_close(g.usb);
    g.usb = NULL;
}

static void on_added(void *ref, io_iterator_t it)
{
    (void)ref;
    io_service_t s;
    int changed = 0;
    while ((s = IOIteratorNext(it))) {
        CFTypeRef v = IORegistryEntryCreateCFProperty(s, CFSTR("locationID"), kCFAllocatorDefault, 0);
        pthread_mutex_lock(&g.lock);
        if (v && CFGetTypeID(v) == CFNumberGetTypeID())
            CFNumberGetValue(v, kCFNumberSInt32Type, &g.location);
        changed = !g.attached;
        g.attached = 1;
        /* If IO is running without the SL1, the watchdog opens it now. */
        pthread_mutex_unlock(&g.lock);
        if (v)
            CFRelease(v);
        IOObjectRelease(s);
    }
    if (changed)
        LOG("SL1 attached (location 0x%08x)", g.location);
}

/*
 * The device stays published through an unplug: while IO runs the clock keeps
 * going, inputs read silence, and the watchdog resumes USB when the SL1 is
 * back. Mixxx keeps its audio running instead of losing the device.
 */
static void on_removed(void *ref, io_iterator_t it)
{
    (void)ref;
    io_service_t s;
    int removed = 0, io = 0;
    while ((s = IOIteratorNext(it))) {
        IOObjectRelease(s);
        pthread_mutex_lock(&g.lock);
        removed |= g.attached;
        g.attached = 0;
        usb_close_locked();
        io = g.io_clients > 0;
        pthread_mutex_unlock(&g.lock);
    }
    if (removed && io)
        LOG("SL1 removed while IO runs: inputs read silence until it is back");
    else if (removed)
        LOG("SL1 removed");
}

static CFMutableDictionaryRef sl1_matching(void)
{
    CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostDevice");
    int vid = SL1_VID, pid = SL1_PID;
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberIntType, &vid);
    CFNumberRef p = CFNumberCreate(NULL, kCFNumberIntType, &pid);
    CFDictionarySetValue(m, CFSTR("idVendor"), v);
    CFDictionarySetValue(m, CFSTR("idProduct"), p);
    CFRelease(v);
    CFRelease(p);
    return m;
}

static void watch_device(void)
{
    g.notify_queue = dispatch_queue_create("org.opensl1.notify", DISPATCH_QUEUE_SERIAL);
    g.notify_port = IONotificationPortCreate(kIOMainPortDefault);
    IONotificationPortSetDispatchQueue(g.notify_port, g.notify_queue);
    IOServiceAddMatchingNotification(g.notify_port, kIOFirstMatchNotification, sl1_matching(),
                                     on_added, NULL, &g.added_iter);
    IOServiceAddMatchingNotification(g.notify_port, kIOTerminatedNotification, sl1_matching(),
                                     on_removed, NULL, &g.removed_iter);
    /* Arm the iterators and pick up a device that is already there. */
    dispatch_sync(g.notify_queue, ^{
        on_added(NULL, g.added_iter);
        on_removed(NULL, g.removed_iter);
    });
}

/* ---- power source --------------------------------------------------------- */

/*
 * Unplugging the Mac's charger made the bus-powered SL1 drop off USB (and once
 * hang). Logging each power source change puts it next to "SL1 removed" in the
 * log, so the two can be matched by their timestamps.
 */
static void log_power(const char *why)
{
    char source[32] = "unknown";
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (info) {
        CFStringRef type = IOPSGetProvidingPowerSourceType(info);
        if (type)
            CFStringGetCString(type, source, sizeof(source), kCFStringEncodingUTF8);
        CFRelease(info);
    }
    int watts = 0;
    CFDictionaryRef adapter = IOPSCopyExternalPowerAdapterDetails();
    if (adapter) {
        CFNumberRef w = CFDictionaryGetValue(adapter, CFSTR(kIOPSPowerAdapterWattsKey));
        if (w && CFGetTypeID(w) == CFNumberGetTypeID())
            CFNumberGetValue(w, kCFNumberIntType, &watts);
        CFRelease(adapter);
    }
    pthread_mutex_lock(&g.lock);
    int attached = g.attached, io = g.io_clients > 0;
    pthread_mutex_unlock(&g.lock);
    os_log(opensl1_log(), "OpenSL1: power %{public}s: %{public}s, adapter %d W (SL1 %{public}s, IO %{public}s)",
           why, source, watts, attached ? "attached" : "absent", io ? "running" : "stopped");
    fprintf(stderr, "OpenSL1: power %s: %s, adapter %d W\n", why, source, watts);
}

static void watch_power(void)
{
    log_power("at load");
    notify_register_dispatch(kIOPSNotifyPowerSource, &g.power_token, g.notify_queue, ^(int token) {
        (void)token;
        log_power("changed");
    });
}

/* ---- IO ------------------------------------------------------------------ */

/* Headroom statistics start at sample `from` (0 for a fresh start). */
static void reset_headroom_locked(int64_t from)
{
    atomic_store(&g.in_margin_min, INT64_MAX);
    atomic_store(&g.out_margin_min, INT64_MAX);
    atomic_store(&g.in_late, 0);
    atomic_store(&g.out_late, 0);
    atomic_store(&g.in_late_last, 0);
    atomic_store(&g.out_late_last, 0);
    g.io_start_sample = from;
}

/*
 * Opens the SL1 and starts all four streams. restart = 1 is the watchdog
 * path: IO is still running in the HAL, so keep the rings and the timeline.
 */
static OSStatus usb_start_locked(int restart)
{
    char err[256];
    sl1_config cfg;
    sl1_config_default(&cfg);
    cfg.packets_per_transfer = USB_PACKETS;
    cfg.transfers = USB_TRANSFERS_IN;
    cfg.out_transfers = USB_TRANSFERS_OUT;
    /* Retry: right after a re-plug or while another program releases the SL1, open can fail. */
    int opened = 0;
    for (int attempt = 0; attempt < 3 && !opened; attempt++) {
        if (attempt)
            usleep(100000);
        if (g.test_open_fail) {
            g.test_open_fail--;
            snprintf(err, sizeof(err), "test: simulated open failure");
            opened = 0;
        } else {
            opened = sl1_open(&g.usb, &cfg, err, sizeof(err)) == 0;
        }
        if (!opened && gate_pass(&gate_open, err))
            LOG_STR("open failed", err);
    }
    if (!opened) {
        g.usb = NULL;
        return kAudioHardwareNotRunningError;
    }
    g.resume = 0;
    if (restart && atomic_load(&g.clock_valid)) {
        clock_read(&g.resume_sample, &g.resume_host, &g.resume_per_tick);
        g.resume = g.resume_per_tick > 0;
    }
    if (!g.resume) {
        memset(g.in_ring, 0, sizeof(g.in_ring));
        memset(g.out_ring, 0, sizeof(g.out_ring));
        atomic_store(&g.b_written, 0);
    }
    g.a_started = g.b_started = 0;
    memset(g.meter, 0, sizeof(g.meter));
    g.out_started[0] = g.out_started[1] = 0;
    memset(g.out_frames, 0, sizeof(g.out_frames));
    memset(g.out_audible, 0, sizeof(g.out_audible));
    g.ref_frame0 = g.ref_host0 = 0;
    g.packets = 0;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    g.ticks_per_frame = 1e6 * tb.denom / tb.numer; /* 1 ms in host ticks */
    uint64_t packets_before = atomic_load(&g.in_packets);
    if (!restart)
        reset_headroom_locked(0);

    sl1_callbacks cb = { on_in, on_out, NULL };
    sl1_set_callbacks(g.usb, &cb);
    unsigned mask = SL1_STREAM_BIT(SL1_STREAM_A_IN) | SL1_STREAM_BIT(SL1_STREAM_B_IN) |
                    SL1_STREAM_BIT(SL1_STREAM_A_OUT) | SL1_STREAM_BIT(SL1_STREAM_B_OUT);
    if (sl1_start(g.usb, mask, err, sizeof(err))) {
        if (gate_pass(&gate_start, err))
            LOG_STR("start failed", err);
        usb_close_locked();
        return kAudioHardwareNotRunningError;
    }
    /* The HAL asks for a zero timestamp right away; wait for the first packets. */
    for (int i = 0; i < 200 && atomic_load(&g.in_packets) == packets_before; i++)
        usleep(2000);
    if (atomic_load(&g.in_packets) == packets_before) {
        if (gate_pass(&gate_start, "no data from Input 1"))
            LOG("no data from Input 1");
        usb_close_locked();
        return kAudioHardwareNotRunningError;
    }
    if (!restart)
        g.seed++;
    /* Taking over from the free-running clock: count headroom from the first SL1 data on. */
    if (g.free_running)
        reset_headroom_locked((int64_t)atomic_load(&g.in_written));
    g.free_running = 0;
    gate_flush(&gate_open);
    gate_flush(&gate_start);
    gate_flush(&gate_watchdog);
    if (restart)
        LOG("IO restarted");
    else
        LOG("IO started");
    return noErr;
}

/*
 * IO without the SL1 (not plugged in, or it fails to open): run the clock at
 * the nominal rate from host time so the app can start and keep its setup.
 * Inputs read silence, outputs go nowhere; the watchdog switches to the SL1
 * on the same timeline once it opens.
 */
static void start_free_clock_locked(void)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    g.ticks_per_frame = 1e6 * tb.denom / tb.numer;
    memset(g.in_ring, 0, sizeof(g.in_ring));
    memset(g.out_ring, 0, sizeof(g.out_ring));
    atomic_store(&g.b_written, 0);
    clock_publish(0, (double)mach_absolute_time(), SL1_RATE / 1000.0 / g.ticks_per_frame);
    atomic_store(&g.clock_valid, 1);
    g.free_running = 1;
    g.seed++;
    LOG("IO started without the SL1: silence until it is available");
}

static void wd_snapshot(void)
{
    for (int i = 0; i < 4; i++)
        g.wd_last[i] = atomic_load_explicit(&g.progress[i], memory_order_relaxed);
}

/*
 * Runs every WATCHDOG_MS on the notify queue. If IO is running but Input 1 has
 * stopped delivering (USB glitch, re-plug, wake from sleep), reopen the SL1
 * instead of staying silent until the app restarts its audio.
 */
static void watchdog_tick(void)
{
    pthread_mutex_lock(&g.lock);
    if (g.io_clients == 0 || !g.attached) {
        g.wd_stall = 0;
        pthread_mutex_unlock(&g.lock);
        return;
    }
    if (++g.wd_ticks % (30000 / WATCHDOG_MS) == 0)
        log_stats_locked(1);
    /* Every stream must move: a dead Input 2 or Line output is restarted too, not only Input 1. */
    unsigned stalled = g.usb ? 0 : 0xf;
    for (int i = 0; i < 4; i++)
        if (atomic_load_explicit(&g.progress[i], memory_order_relaxed) == g.wd_last[i])
            stalled |= 1u << i;
    if (!stalled) {
        wd_snapshot();
        g.wd_stall = 0;
    } else if (++g.wd_stall >= WATCHDOG_STALL_TICKS) {
        g.restarts++;
        if (gate_pass(&gate_watchdog, "no USB data"))
            LOG("no USB data for %d ms (streams 0x%x: 1 = Line 1, 2 = Input 1, 4 = Line 2, 8 = Input 2), "
                "restarting (restart %u)",
                g.wd_stall * WATCHDOG_MS, stalled, g.restarts);
        usb_close_locked();
        usb_start_locked(1);
        wd_snapshot();
        g.wd_stall = 0;
    }
    pthread_mutex_unlock(&g.lock);
}

static void start_watchdog(void)
{
    g.watchdog = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, g.notify_queue);
    dispatch_source_set_timer(g.watchdog, dispatch_time(DISPATCH_TIME_NOW, WATCHDOG_MS * NSEC_PER_MSEC),
                              WATCHDOG_MS * NSEC_PER_MSEC, 10 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(g.watchdog, ^{
        watchdog_tick();
    });
    dispatch_resume(g.watchdog);
}

/* ---- COM plumbing -------------------------------------------------------- */

static AudioServerPlugInDriverInterface g_vtable;
static AudioServerPlugInDriverInterface *g_vtable_ptr = &g_vtable;
static AudioServerPlugInDriverRef g_driver = &g_vtable_ptr;
static _Atomic UInt32 g_refcount = 1;

static HRESULT drv_QueryInterface(void *self, REFIID iid, LPVOID *out)
{
    CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, iid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID)) {
        atomic_fetch_add(&g_refcount, 1);
        *out = self;
        r = S_OK;
    } else {
        *out = NULL;
    }
    CFRelease(req);
    return r;
}

static ULONG drv_AddRef(void *self)
{
    (void)self;
    return atomic_fetch_add(&g_refcount, 1) + 1;
}

static ULONG drv_Release(void *self)
{
    (void)self;
    UInt32 n = atomic_load(&g_refcount);
    if (n > 0)
        n = atomic_fetch_sub(&g_refcount, 1) - 1;
    return n;
}

static OSStatus drv_Initialize(AudioServerPlugInDriverRef d, AudioServerPlugInHostRef host)
{
    (void)d;
    g.host = host;
    /*
     * Always published, also without the SL1: an app that starts while the
     * SL1 is missing (or hangs after a power glitch) keeps its device setup
     * and gets sound as soon as the SL1 is back.
     */
    g.present = 1;
    const char *stall = getenv("OPENSL1_TEST_STALL");
    if (stall)
        g.test_stall_at = strtoull(stall, NULL, 10);
    const char *stall_b = getenv("OPENSL1_TEST_STALL_B");
    if (stall_b)
        g.test_stall_b_at = strtoull(stall_b, NULL, 10);
    const char *open_fail = getenv("OPENSL1_TEST_OPEN_FAIL");
    if (open_fail)
        g.test_open_fail = (unsigned)strtoul(open_fail, NULL, 10);
    watch_device();
    watch_power();
    start_watchdog();
    LOG("loaded");
    return noErr;
}

static OSStatus drv_CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc,
                                 const AudioServerPlugInClientInfo *ci, AudioObjectID *out)
{
    (void)d, (void)desc, (void)ci, (void)out;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus drv_DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID id)
{
    (void)d, (void)id;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus drv_AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id,
                                    const AudioServerPlugInClientInfo *ci)
{
    (void)d, (void)ci;
    return id == OBJ_DEVICE ? noErr : kAudioHardwareBadObjectError;
}

static OSStatus drv_RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id,
                                       const AudioServerPlugInClientInfo *ci)
{
    (void)d, (void)ci;
    return id == OBJ_DEVICE ? noErr : kAudioHardwareBadObjectError;
}

static OSStatus drv_PerformConfigChange(AudioServerPlugInDriverRef d, AudioObjectID id,
                                        UInt64 action, void *info)
{
    (void)d, (void)id, (void)action, (void)info;
    return noErr;
}

static OSStatus drv_AbortConfigChange(AudioServerPlugInDriverRef d, AudioObjectID id, UInt64 action,
                                      void *info)
{
    (void)d, (void)id, (void)action, (void)info;
    return noErr;
}

/* ---- properties ---------------------------------------------------------- */

static void fill_format(AudioStreamBasicDescription *f)
{
    memset(f, 0, sizeof(*f));
    f->mSampleRate = SL1_RATE;
    f->mFormatID = kAudioFormatLinearPCM;
    f->mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    f->mBytesPerPacket = 4 * CHANNELS;
    f->mFramesPerPacket = 1;
    f->mBytesPerFrame = 4 * CHANNELS;
    f->mChannelsPerFrame = CHANNELS;
    f->mBitsPerChannel = 32;
}

/* Lock-free: the watchdog can hold g.lock for seconds while a hung SL1 is reopened. */
static int device_visible(void)
{
    return atomic_load(&g.present);
}

static int object_exists(AudioObjectID id)
{
    if (id == OBJ_PLUGIN)
        return 1;
    if (id == OBJ_DEVICE || id == OBJ_STREAM_IN || id == OBJ_STREAM_OUT)
        return device_visible();
    return 0;
}

/*
 * The SL1 has no serial number and we publish at most one device, so the UID
 * does not depend on the USB port: apps keep their settings after re-plugging.
 */
static CFStringRef device_uid(void)
{
    return CFStringCreateCopy(NULL, CFSTR(MODEL_UID ":1"));
}

/* OwnedObjects may carry a qualifier: a list of class IDs the caller wants. */
static int class_wanted(AudioClassID cls, UInt32 qsize, const void *qdata)
{
    if (!qdata || qsize < sizeof(AudioClassID))
        return 1;
    const AudioClassID *ids = qdata;
    for (UInt32 i = 0; i < qsize / sizeof(AudioClassID); i++)
        if (ids[i] == cls || ids[i] == kAudioObjectClassID)
            return 1;
    return 0;
}

/*
 * One function answers both "how big" (out == NULL) and "give me the value".
 * Returns kAudioHardwareUnknownPropertyError for properties we don't have.
 */
static OSStatus get_prop(AudioObjectID obj, const AudioObjectPropertyAddress *a, UInt32 qsize,
                         const void *qdata, UInt32 cap, UInt32 *size, void *out)
{
#define PUT(type, value)                                                                          \
    do {                                                                                          \
        if (out) {                                                                                \
            if (cap < sizeof(type))                                                               \
                return kAudioHardwareBadPropertySizeError;                                        \
            *(type *)out = (value);                                                               \
        }                                                                                         \
        *size = sizeof(type);                                                                     \
        return noErr;                                                                             \
    } while (0)
#define PUT_IDS(list, count)                                                                      \
    do {                                                                                          \
        UInt32 n_ = (count);                                                                      \
        if (out) {                                                                                \
            UInt32 room_ = cap / sizeof(AudioObjectID);                                           \
            if (n_ > room_)                                                                       \
                n_ = room_;                                                                       \
            memcpy(out, (list), n_ * sizeof(AudioObjectID));                                      \
        }                                                                                         \
        *size = n_ * sizeof(AudioObjectID);                                                       \
        return noErr;                                                                             \
    } while (0)

    AudioObjectPropertyScope scope = a->mScope;
    int present = device_visible();

    switch (obj) {
    case OBJ_PLUGIN: {
        AudioObjectID dev[1] = { OBJ_DEVICE };
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: PUT(CFStringRef, CFSTR("OpenSL1"));
        case kAudioObjectPropertyOwnedObjects:
            PUT_IDS(dev, present && class_wanted(kAudioDeviceClassID, qsize, qdata) ? 1 : 0);
        case kAudioPlugInPropertyDeviceList: PUT_IDS(dev, present ? 1 : 0);
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            AudioObjectID r = kAudioObjectUnknown;
            if (out && qsize == sizeof(CFStringRef) && qdata && *(const CFStringRef *)qdata && present) {
                CFStringRef uid = device_uid();
                if (CFEqual(*(const CFStringRef *)qdata, uid))
                    r = OBJ_DEVICE;
                CFRelease(uid);
            }
            PUT(AudioObjectID, r);
        }
        case kAudioPlugInPropertyResourceBundle: PUT(CFStringRef, CFSTR(""));
        }
        break;
    }
    case OBJ_DEVICE: {
        AudioObjectID both[2] = { OBJ_STREAM_IN, OBJ_STREAM_OUT };
        AudioObjectID in[1] = { OBJ_STREAM_IN }, outs[1] = { OBJ_STREAM_OUT };
        const AudioObjectID *streams = scope == kAudioObjectPropertyScopeInput    ? in
                                       : scope == kAudioObjectPropertyScopeOutput ? outs
                                                                                  : both;
        UInt32 nstreams = scope == kAudioObjectPropertyScopeGlobal ? 2 : 1;
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, OBJ_PLUGIN);
        case kAudioObjectPropertyName: PUT(CFStringRef, CFSTR("Rane SL1"));
        case kAudioObjectPropertyManufacturer: PUT(CFStringRef, CFSTR("Rane"));
        case kAudioObjectPropertyModelName: PUT(CFStringRef, CFSTR("SL1 (OpenSL1 driver)"));
        case kAudioObjectPropertyOwnedObjects:
            PUT_IDS(streams, class_wanted(kAudioStreamClassID, qsize, qdata) ? nstreams : 0);
        case kAudioDevicePropertyStreams: PUT_IDS(streams, nstreams);
        case kAudioObjectPropertyControlList: PUT_IDS(both, 0);
        case kAudioDevicePropertyDeviceUID: {
            if (!out || cap < sizeof(CFStringRef)) {
                *size = sizeof(CFStringRef);
                return out ? kAudioHardwareBadPropertySizeError : noErr;
            }
            PUT(CFStringRef, device_uid()); /* caller releases */
        }
        case kAudioDevicePropertyModelUID: PUT(CFStringRef, CFSTR(MODEL_UID));
        case kAudioDevicePropertyTransportType: PUT(UInt32, kAudioDeviceTransportTypeUSB);
        case kAudioDevicePropertyRelatedDevices: {
            AudioObjectID self[1] = { OBJ_DEVICE };
            PUT_IDS(self, 1);
        }
        case kAudioDevicePropertyClockDomain: PUT(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: PUT(UInt32, (UInt32)present);
        case kAudioDevicePropertyDeviceIsRunning: PUT(UInt32, atomic_load(&g.io_clients) > 0);
        /* Never the system default: alerts and other apps' sound must not end up on the PA. */
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: PUT(UInt32, 0);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: PUT(UInt32, 0);
        case kAudioDevicePropertyLatency:
            PUT(UInt32, scope == kAudioObjectPropertyScopeInput ? LATENCY_IN : LATENCY_OUT);
        case kAudioDevicePropertySafetyOffset:
            PUT(UInt32, scope == kAudioObjectPropertyScopeInput ? SAFETY_IN : SAFETY_OUT);
        case kAudioDevicePropertyNominalSampleRate: PUT(Float64, SL1_RATE);
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            AudioValueRange r = { SL1_RATE, SL1_RATE };
            PUT(AudioValueRange, r);
        }
        case kAudioDevicePropertyIsHidden: PUT(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: PUT(UInt32, RING_FRAMES);
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            if (out) {
                if (cap < 2 * sizeof(UInt32))
                    return kAudioHardwareBadPropertySizeError;
                ((UInt32 *)out)[0] = 1;
                ((UInt32 *)out)[1] = 2;
            }
            *size = 2 * sizeof(UInt32);
            return noErr;
        }
        case kAudioObjectPropertyElementName: {
            if (scope == kAudioObjectPropertyScopeGlobal || a->mElement < 1 || a->mElement > CHANNELS)
                break;
            const char *name = (scope == kAudioObjectPropertyScopeInput ? CHANNEL_NAMES_IN
                                                                        : CHANNEL_NAMES_OUT)[a->mElement - 1];
            if (!out || cap < sizeof(CFStringRef)) {
                *size = sizeof(CFStringRef);
                return out ? kAudioHardwareBadPropertySizeError : noErr;
            }
            PUT(CFStringRef, CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8));
        }
        }
        break;
    }
    case OBJ_STREAM_IN:
    case OBJ_STREAM_OUT: {
        int is_in = obj == OBJ_STREAM_IN;
        AudioStreamBasicDescription f;
        fill_format(&f);
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, OBJ_DEVICE);
        case kAudioObjectPropertyOwnedObjects: PUT_IDS(&obj, 0);
        case kAudioObjectPropertyName:
            PUT(CFStringRef, is_in ? CFSTR("Inputs 1+2") : CFSTR("Lines 1+2"));
        case kAudioStreamPropertyIsActive: PUT(UInt32, atomic_load(&g.stream_active[is_in ? 0 : 1]));
        case kAudioStreamPropertyDirection: PUT(UInt32, is_in ? 1 : 0);
        case kAudioStreamPropertyTerminalType:
            PUT(UInt32, is_in ? kAudioStreamTerminalTypeLine : kAudioStreamTerminalTypeLine);
        case kAudioStreamPropertyStartingChannel: PUT(UInt32, 1);
        case kAudioStreamPropertyLatency: PUT(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: PUT(AudioStreamBasicDescription, f);
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            AudioStreamRangedDescription r = { f, { SL1_RATE, SL1_RATE } };
            PUT(AudioStreamRangedDescription, r);
        }
        }
        break;
    }
    default:
        return kAudioHardwareBadObjectError;
    }
    return kAudioHardwareUnknownPropertyError;
#undef PUT
#undef PUT_IDS
}

/* Logs each property we don't implement once, to see what the HAL and apps ask for. */
static void note_unknown(AudioObjectID obj, const AudioObjectPropertyAddress *a)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static uint32_t seen[64];
    static unsigned nseen;
    uint32_t key = a->mSelector ^ (obj << 28);
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < nseen; i++)
        if (seen[i] == key) {
            pthread_mutex_unlock(&lock);
            return;
        }
    if (nseen < 64)
        seen[nseen++] = key;
    pthread_mutex_unlock(&lock);
    char sel[5] = { (char)(a->mSelector >> 24), (char)(a->mSelector >> 16), (char)(a->mSelector >> 8),
                    (char)a->mSelector, 0 };
    for (int i = 0; i < 4; i++)
        if (sel[i] < 32 || sel[i] > 126)
            sel[i] = '?';
    LOG_STR("unknown property", sel);
    (void)obj;
}

static Boolean drv_HasProperty(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                               const AudioObjectPropertyAddress *a)
{
    (void)d, (void)pid;
    UInt32 size;
    if (!object_exists(obj))
        return false;
    /* Device UID and element names allocate; ask for the size only. */
    OSStatus r = get_prop(obj, a, 0, NULL, 0, &size, NULL);
    if (r == kAudioHardwareUnknownPropertyError)
        note_unknown(obj, a);
    return r == noErr;
}

static OSStatus drv_IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                       const AudioObjectPropertyAddress *a, Boolean *out)
{
    (void)d, (void)pid;
    UInt32 size;
    if (!object_exists(obj))
        return kAudioHardwareBadObjectError;
    OSStatus r = get_prop(obj, a, 0, NULL, 0, &size, NULL);
    if (r)
        return r;
    switch (a->mSelector) {
    case kAudioDevicePropertyNominalSampleRate:
    case kAudioStreamPropertyIsActive:
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat:
        *out = true;
        break;
    default:
        *out = false;
    }
    return noErr;
}

static OSStatus drv_GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                        const AudioObjectPropertyAddress *a, UInt32 qsize,
                                        const void *qdata, UInt32 *out)
{
    (void)d, (void)pid;
    if (!object_exists(obj))
        return kAudioHardwareBadObjectError;
    return get_prop(obj, a, qsize, qdata, 0, out, NULL);
}

static OSStatus drv_GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                    const AudioObjectPropertyAddress *a, UInt32 qsize,
                                    const void *qdata, UInt32 cap, UInt32 *size, void *out)
{
    (void)d, (void)pid;
    if (!object_exists(obj))
        return kAudioHardwareBadObjectError;
    return get_prop(obj, a, qsize, qdata, cap, size, out);
}

static OSStatus drv_SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                    const AudioObjectPropertyAddress *a, UInt32 qsize,
                                    const void *qdata, UInt32 size, const void *data)
{
    (void)d, (void)pid, (void)qsize, (void)qdata;
    if (!object_exists(obj))
        return kAudioHardwareBadObjectError;
    switch (a->mSelector) {
    case kAudioDevicePropertyNominalSampleRate:
        if (obj != OBJ_DEVICE)
            return kAudioHardwareUnknownPropertyError;
        if (!data || size < sizeof(Float64))
            return kAudioHardwareBadPropertySizeError;
        return *(const Float64 *)data == SL1_RATE ? noErr : kAudioHardwareIllegalOperationError;
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat: {
        if (obj != OBJ_STREAM_IN && obj != OBJ_STREAM_OUT)
            return kAudioHardwareUnknownPropertyError;
        if (!data || size < sizeof(AudioStreamBasicDescription))
            return kAudioHardwareBadPropertySizeError;
        const AudioStreamBasicDescription *f = data;
        AudioStreamBasicDescription ours;
        fill_format(&ours);
        return f->mSampleRate == ours.mSampleRate && f->mFormatID == ours.mFormatID &&
                       f->mChannelsPerFrame == ours.mChannelsPerFrame &&
                       f->mBitsPerChannel == ours.mBitsPerChannel
                   ? noErr
                   : kAudioDeviceUnsupportedFormatError;
    }
    case kAudioStreamPropertyIsActive:
        if (obj != OBJ_STREAM_IN && obj != OBJ_STREAM_OUT)
            return kAudioHardwareUnknownPropertyError;
        if (!data || size < sizeof(UInt32))
            return kAudioHardwareBadPropertySizeError;
        atomic_store(&g.stream_active[obj == OBJ_STREAM_IN ? 0 : 1], *(const UInt32 *)data != 0);
        notify_changed(obj, kAudioStreamPropertyIsActive);
        return noErr;
    }
    return kAudioHardwareUnknownPropertyError;
}

/* ---- IO entry points ------------------------------------------------------ */

/*
 * StartIO/StopIO do not send DeviceIsRunning notifications themselves: the HAL
 * tracks that state, and notifying from inside StartIO is what OpenSL3 removed
 * (it saw coreaudiod spin after a few start/stop cycles).
 */
static OSStatus drv_StartIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client)
{
    (void)d, (void)client;
    if (dev != OBJ_DEVICE)
        return kAudioHardwareBadObjectError;
    OSStatus r = noErr;
    pthread_mutex_lock(&g.lock);
    if (g.io_clients == 0) {
        atomic_store(&g.clock_valid, 0);
        if (!g.attached || usb_start_locked(0) != noErr)
            start_free_clock_locked();
        wd_snapshot();
        g.wd_stall = 0;
    }
    if (r == noErr)
        g.io_clients++;
    pthread_mutex_unlock(&g.lock);
    return r;
}

static OSStatus drv_StopIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client)
{
    (void)d, (void)client;
    if (dev != OBJ_DEVICE)
        return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&g.lock);
    if (g.io_clients > 0 && --g.io_clients == 0) {
        usb_close_locked();
        atomic_store(&g.clock_valid, 0);
        LOG("IO stopped");
    }
    pthread_mutex_unlock(&g.lock);
    return noErr;
}

static OSStatus drv_GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client,
                                     Float64 *sample_time, UInt64 *host_time, UInt64 *seed)
{
    (void)d, (void)client;
    if (dev != OBJ_DEVICE)
        return kAudioHardwareBadObjectError;
    if (!atomic_load(&g.clock_valid))
        return kAudioHardwareNotRunningError;
    double s, h, per_tick;
    clock_read(&s, &h, &per_tick);
    double now = (double)mach_absolute_time();
    double s_now = s + (now - h) * per_tick;
    double zero = floor(s_now / RING_FRAMES) * RING_FRAMES;
    *sample_time = zero;
    *host_time = (UInt64)(h + (zero - s) / per_tick);
    *seed = atomic_load(&g.seed);
    return noErr;
}

static OSStatus drv_WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client,
                                      UInt32 op, Boolean *will, Boolean *in_place)
{
    (void)d, (void)dev, (void)client;
    *will = op == kAudioServerPlugInIOOperationReadInput || op == kAudioServerPlugInIOOperationWriteMix;
    *in_place = true;
    return noErr;
}

static OSStatus drv_BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client,
                                     UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *ci)
{
    (void)d, (void)dev, (void)client, (void)op, (void)frames, (void)ci;
    return noErr;
}

/* IO thread: lock-free minimum (single writer, so load + store is enough). */
static void track_min(_Atomic int64_t *min, _Atomic uint64_t *late, _Atomic int64_t *last, int64_t m,
                      int64_t pos)
{
    if (m < atomic_load_explicit(min, memory_order_relaxed))
        atomic_store_explicit(min, m, memory_order_relaxed);
    if (m < 0) {
        atomic_fetch_add_explicit(late, 1, memory_order_relaxed);
        atomic_store_explicit(last, pos, memory_order_relaxed);
    }
}

static OSStatus drv_DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, AudioObjectID stream,
                                  UInt32 client, UInt32 op, UInt32 frames,
                                  const AudioServerPlugInIOCycleInfo *ci, void *main, void *secondary)
{
    (void)d, (void)client, (void)secondary;
    if (dev != OBJ_DEVICE)
        return kAudioHardwareBadObjectError;
    float *buf = main;
    if (!buf || !ci)
        return noErr;
    if (op == kAudioServerPlugInIOOperationReadInput && stream == OBJ_STREAM_IN) {
        int64_t pos = (int64_t)ci->mInputTime.mSampleTime;
        int64_t written = (int64_t)atomic_load_explicit(&g.in_written, memory_order_acquire);
        int64_t written_b = (int64_t)atomic_load_explicit(&g.b_written, memory_order_acquire);
        int64_t m = written - (pos + (int64_t)frames);
        track_min(&g.in_margin_min, &g.in_late, &g.in_late_last, m, pos);
        for (UInt32 i = 0; i < frames; i++, pos++) {
            /* Not yet received (unplugged, USB stalled) or already overwritten: silence,
             * never old timecode from a previous lap of the ring. Each input has its own
             * counter, so a lagging or dead Input 2 reads silence too. */
            int fresh_a = pos >= 0 && pos < written && pos >= written - (RING_FRAMES - 1024);
            int fresh_b = pos >= 0 && pos < written_b && pos >= written_b - (RING_FRAMES - 1024);
            const int16_t *src = &g.in_ring[((uint64_t)pos & RING_MASK) * CHANNELS];
            for (int c = 0; c < CHANNELS; c++)
                buf[i * CHANNELS + (UInt32)c] = (c < 2 ? fresh_a : fresh_b) ? src[c] * (1.0f / 32768.0f) : 0.0f;
        }
    } else if (op == kAudioServerPlugInIOOperationWriteMix && stream == OBJ_STREAM_OUT) {
        int64_t pos = (int64_t)ci->mOutputTime.mSampleTime;
        int64_t m = pos - (int64_t)atomic_load_explicit(&g.out_taken, memory_order_acquire);
        track_min(&g.out_margin_min, &g.out_late, &g.out_late_last, m, pos);
        for (UInt32 i = 0; i < frames; i++, pos++) {
            if (pos < 0)
                continue;
            int16_t *dst = &g.out_ring[((uint64_t)pos & RING_MASK) * CHANNELS];
            for (int c = 0; c < CHANNELS; c++) {
                float v = buf[i * CHANNELS + (UInt32)c] * 32768.0f;
                dst[c] = v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int16_t)lrintf(v);
            }
        }
    }
    return noErr;
}

static OSStatus drv_EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client,
                                   UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *ci)
{
    (void)d, (void)dev, (void)client, (void)op, (void)frames, (void)ci;
    return noErr;
}

static AudioServerPlugInDriverInterface g_vtable = {
    NULL,
    drv_QueryInterface,
    drv_AddRef,
    drv_Release,
    drv_Initialize,
    drv_CreateDevice,
    drv_DestroyDevice,
    drv_AddDeviceClient,
    drv_RemoveDeviceClient,
    drv_PerformConfigChange,
    drv_AbortConfigChange,
    drv_HasProperty,
    drv_IsPropertySettable,
    drv_GetPropertyDataSize,
    drv_GetPropertyData,
    drv_SetPropertyData,
    drv_StartIO,
    drv_StopIO,
    drv_GetZeroTimeStamp,
    drv_WillDoIOOperation,
    drv_BeginIOOperation,
    drv_DoIOOperation,
    drv_EndIOOperation,
};

/* Factory named in Info.plist (CFPlugInFactories). */
__attribute__((visibility("default"))) void *OpenSL1_Create(CFAllocatorRef alloc, CFUUIDRef type)
{
    (void)alloc;
    if (!CFEqual(type, kAudioServerPlugInTypeUUID))
        return NULL;
    return g_driver;
}
