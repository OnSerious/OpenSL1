/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * OpenSL1 - IOUSBHost backend for macOS (same API as the libusb backend).
 *
 * libusb's macOS backend goes through the legacy IOUSBLib interface; with the
 * SL1 on macOS 27.0.1 its alt-setting path triggered a kernel panic (see
 * docs/MACOS.md). This backend talks to IOUSBHostInterface/Pipe directly.
 */
#include "sl1_usb.h"

#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <IOUSBHost/IOUSBHost.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "sl1_pkt.h"

/* Frames between "now" and the first scheduled packet. */
#define SL1_START_MARGIN 8

typedef struct stream stream;

typedef struct {
    stream *owner;
    NSMutableData *data;
    IOUSBHostIsochronousTransaction *tx;
    uint64_t first_frame; /* bus frame of tx[0] */
} xfer_slot;

struct stream {
    sl1_device *dev;
    sl1_stream_id id;
    const sl1_stream_info *info;
    int active;
    sl1_ring ring;
    sl1_pkt_clock clock; /* OUT only */
    IOUSBHostInterface *intf;
    IOUSBHostPipe *pipe;
    xfer_slot *slots;
    int nslots;
    uint64_t next_frame; /* only touched on dev->queue */
    _Atomic int in_flight;
    pthread_mutex_t stats_lock;
    sl1_stream_stats stats;
    int16_t *scratch; /* OUT: frames pulled from the ring for one packet */
};

struct sl1_device {
    sl1_config cfg;
    dispatch_queue_t queue;
    IOUSBHostDevice *usbdev; /* only held if we had to set the configuration */
    sl1_callbacks cb;        /* if set, used instead of the rings */
    stream streams[SL1_STREAM_COUNT];
    _Atomic int running;
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

/* OPENSL1_TRACE=<file>: each step is synced to disk, so it survives a kernel panic. */
static void trace(const char *fmt, ...)
{
    static int fd = -2;
    if (fd == -2) {
        const char *path = getenv("OPENSL1_TRACE");
        fd = path ? open(path, O_WRONLY | O_CREAT | O_APPEND, 0644) : -1;
    }
    if (fd < 0)
        return;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(line) - 2)
        n = (int)sizeof(line) - 2;
    line[n++] = '\n';
    write(fd, line, (size_t)n);
    fsync(fd);
}

static const char *nserr(NSError *e)
{
    return e ? [[NSString stringWithFormat:@"%@ (0x%lx)", e.localizedDescription,
                                           (unsigned long)e.code] UTF8String]
             : "unknown error";
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

/* Each packet has a fixed slot of max_packet bytes in the buffer. */
static void fill_slot(stream *s, xfer_slot *slot)
{
    int n = s->dev->cfg.packets_per_transfer;
    unsigned char *base = slot->data.mutableBytes;
    for (int i = 0; i < n; i++) {
        IOUSBHostIsochronousTransaction *t = &slot->tx[i];
        memset(t, 0, sizeof(*t));
        t->offset = (uint32_t)(i * s->info->max_packet);
        if (s->info->is_input) {
            t->requestCount = (uint32_t)s->info->max_packet;
            continue;
        }
        int ch = s->info->channels;
        size_t frame_bytes = (size_t)ch * SL1_BYTES_PER_SAMPLE;
        unsigned max_frames = (unsigned)(s->info->max_packet / (int)frame_bytes);
        if (s->dev->cb.out_packet) {
            unsigned frames = s->dev->cb.out_packet(s->dev->cb.ctx, s->id, (int16_t *)(base + t->offset),
                                                    max_frames, slot->first_frame + (uint64_t)i);
            if (frames > max_frames)
                frames = max_frames;
            t->requestCount = (uint32_t)(frames * frame_bytes);
            continue;
        }
        unsigned frames = sl1_pkt_clock_next(&s->clock);
        size_t got = sl1_ring_read(&s->ring, s->scratch, frames);
        if (got < frames) {
            memset(s->scratch + got * (size_t)ch, 0, (frames - got) * frame_bytes);
            stats_xrun(s);
        }
        memcpy(base + t->offset, s->scratch, frames * frame_bytes);
        t->requestCount = (uint32_t)(frames * frame_bytes);
    }
}

static void slot_done(xfer_slot *slot, IOReturn status, IOUSBHostIsochronousTransaction *tx);

/* Runs on dev->queue. Fills the slot and schedules it at next_frame, resyncing once if we
 * fell behind. */
static int submit(stream *s, xfer_slot *slot)
{
    int n = s->dev->cfg.packets_per_transfer;
    for (int attempt = 0; attempt < 2; attempt++) {
        uint64_t now = [s->intf frameNumberWithTime:NULL];
        if (s->next_frame <= now + 1)
            s->next_frame = now + SL1_START_MARGIN;
        slot->first_frame = s->next_frame;
        if (attempt == 0 || s->info->is_input || s->dev->cb.out_packet)
            fill_slot(s, slot);
        NSError *e = nil;
        BOOL ok = [s->pipe enqueueIORequestWithData:slot->data
                                    transactionList:slot->tx
                               transactionListCount:(NSUInteger)n
                                   firstFrameNumber:s->next_frame
                                            options:IOUSBHostIsochronousTransferOptionsNone
                                              error:&e
                                  completionHandler:^(IOReturn status,
                                                      IOUSBHostIsochronousTransaction *tx) {
                                      slot_done(slot, status, tx);
                                  }];
        if (ok) {
            s->next_frame += (uint64_t)n;
            return 0;
        }
        s->next_frame = 0;
    }
    return -1;
}

static void slot_done(xfer_slot *slot, IOReturn status, IOUSBHostIsochronousTransaction *tx)
{
    stream *s = slot->owner;
    sl1_device *dev = s->dev;
    size_t frame_bytes = (size_t)s->info->channels * SL1_BYTES_PER_SAMPLE;
    const unsigned char *base = slot->data.bytes;

    if (status != kIOReturnAborted && status != kIOReturnNotResponding) {
        for (int i = 0; i < dev->cfg.packets_per_transfer; i++) {
            int ok = tx[i].status == kIOReturnSuccess;
            if (s->info->is_input) {
                /* Never trust a length beyond the packet's slot in the buffer. */
                uint32_t bytes = tx[i].completeCount;
                if (bytes > (uint32_t)s->info->max_packet)
                    bytes = (uint32_t)s->info->max_packet;
                unsigned frames = ok ? bytes / frame_bytes : 0;
                if (dev->cb.in_packet)
                    dev->cb.in_packet(dev->cb.ctx, s->id, (const int16_t *)(base + tx[i].offset), frames,
                                      slot->first_frame + (uint64_t)i);
                else if (frames &&
                    sl1_ring_write(&s->ring, (const int16_t *)(base + tx[i].offset), frames) < frames)
                    stats_xrun(s);
                stats_packet(s, frames, ok);
            } else {
                stats_packet(s, (unsigned)(tx[i].requestCount / frame_bytes), ok);
            }
        }
    }

    if (atomic_load(&dev->running) && status != kIOReturnAborted &&
        status != kIOReturnNotResponding && status != kIOReturnNoDevice) {
        if (submit(s, slot) == 0)
            return;
        pthread_mutex_lock(&s->stats_lock);
        s->stats.resubmit_fail++;
        pthread_mutex_unlock(&s->stats_lock);
    }
    atomic_fetch_sub(&s->in_flight, 1);
}

static io_service_t find_interface(int number)
{
    CFMutableDictionaryRef match =
        [IOUSBHostInterface createMatchingDictionaryWithVendorID:@(SL1_VID)
                                                       productID:@(SL1_PID)
                                                       bcdDevice:nil
                                                 interfaceNumber:@(number)
                                              configurationValue:@(SL1_CONFIGURATION)
                                                  interfaceClass:nil
                                               interfaceSubclass:nil
                                               interfaceProtocol:nil
                                                           speed:nil
                                                  productIDArray:nil];
    return IOServiceGetMatchingService(kIOMainPortDefault, match); /* consumes match */
}

/* At most one device reset in this interval, so an incomplete device is not reset in a loop. */
#define SL1_RESET_INTERVAL_NS (10ull * NSEC_PER_SEC)
static uint64_t last_reset_ns;

/*
 * Test hook: OPENSL1_TEST_RESET=<n> makes the n-th call of ensure_configured
 * treat the interfaces as missing, which exercises the reset path.
 */
static int test_incomplete(void)
{
    static long countdown = -1;
    if (countdown == -1) {
        const char *v = getenv("OPENSL1_TEST_RESET");
        countdown = v ? strtol(v, NULL, 10) : 0;
    }
    return countdown > 0 && --countdown == 0;
}

/* Opens the IOUSBHostDevice (needed to configure or reset the SL1). */
static int open_usbdev(sl1_device *dev, char *err, size_t errlen)
{
    if (dev->usbdev)
        return 0;
    CFMutableDictionaryRef match =
        [IOUSBHostDevice createMatchingDictionaryWithVendorID:@(SL1_VID)
                                                    productID:@(SL1_PID)
                                                    bcdDevice:nil
                                                  deviceClass:nil
                                               deviceSubclass:nil
                                               deviceProtocol:nil
                                                        speed:nil
                                               productIDArray:nil];
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, match);
    if (!svc) {
        set_err(err, errlen, "SL1 (%04x:%04x) not found", SL1_VID, SL1_PID);
        return -1;
    }
    NSError *e = nil;
    trace("open device");
    dev->usbdev = [[IOUSBHostDevice alloc] initWithIOService:svc
                                                     options:IOUSBHostObjectInitOptionsNone
                                                       queue:dev->queue
                                                       error:&e
                                             interestHandler:nil];
    IOObjectRelease(svc);
    if (!dev->usbdev) {
        set_err(err, errlen, "open device: %s", nserr(e));
        return -1;
    }
    return 0;
}

/*
 * Re-enumerates the SL1 (like a re-plug); the caller retries later. Seen after
 * the SL1 dropped off the bus when the Mac's charger was unplugged: it came
 * back with only interface 0 published, not answering SET_CONFIGURATION, or
 * (seconds later, still on the bus) not answering SET_INTERFACE, and only a
 * re-plug helped. Always returns -1.
 */
static int reset_device(sl1_device *dev, const char *why, char *err, size_t errlen)
{
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    if (last_reset_ns && now - last_reset_ns < SL1_RESET_INTERVAL_NS) {
        set_err(err, errlen, "%s", why);
        return -1;
    }
    last_reset_ns = now;
    char open_err[128];
    if (open_usbdev(dev, open_err, sizeof(open_err))) {
        set_err(err, errlen, "%s; reset: %s", why, open_err);
        return -1;
    }
    trace("reset device");
    NSError *e = nil;
    if (![dev->usbdev resetWithError:&e])
        set_err(err, errlen, "%s; reset failed: %s", why, nserr(e));
    else
        set_err(err, errlen, "%s; device reset", why);
    return -1;
}

/*
 * Sets configuration 1 if the device has no interfaces yet (fresh plug-in).
 * If that fails or they still do not appear, resets (re-enumerates) the device.
 */
static int ensure_configured(sl1_device *dev, char *err, size_t errlen)
{
    int fake_incomplete = test_incomplete();
    io_service_t probe = fake_incomplete ? 0 : find_interface(SL1_STREAMS[0].interface);
    if (probe) {
        IOObjectRelease(probe);
        return 0;
    }
    if (open_usbdev(dev, err, errlen))
        return -1;
    NSError *e = nil;
    trace("set configuration %d", SL1_CONFIGURATION);
    if (![dev->usbdev configureWithValue:SL1_CONFIGURATION matchInterfaces:YES error:&e]) {
        char why[160];
        snprintf(why, sizeof(why), "set configuration %d: %s", SL1_CONFIGURATION, nserr(e));
        return reset_device(dev, why, err, errlen);
    }
    /* Interfaces are published asynchronously after SET_CONFIGURATION. */
    for (int tries = 0; tries < 100 && !fake_incomplete; tries++) {
        probe = find_interface(SL1_STREAMS[0].interface);
        if (probe) {
            IOObjectRelease(probe);
            return 0;
        }
        usleep(20000);
    }
    return reset_device(dev, "interfaces did not appear after set configuration", err, errlen);
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
    dispatch_queue_attr_t attr =
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
    dev->queue = dispatch_queue_create("org.opensl1.usb", attr);

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
    if (ensure_configured(dev, err, errlen)) {
        sl1_close(dev);
        return -1;
    }
    for (int i = 0; i < SL1_STREAM_COUNT; i++) {
        stream *s = &dev->streams[i];
        io_service_t svc = find_interface(s->info->interface);
        if (!svc) {
            set_err(err, errlen, "interface %d not found", s->info->interface);
            sl1_close(dev);
            return -1;
        }
        NSError *e = nil;
        trace("open interface %d", s->info->interface);
        s->intf = [[IOUSBHostInterface alloc] initWithIOService:svc
                                                        options:IOUSBHostObjectInitOptionsNone
                                                          queue:dev->queue
                                                          error:&e
                                                interestHandler:nil];
        IOObjectRelease(svc);
        if (!s->intf) {
            set_err(err, errlen, "open interface %d: %s", s->info->interface, nserr(e));
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
    NSError *e = nil;
    trace("%s: select alt %d on interface %d", s->info->name, SL1_ALT_ACTIVE, s->info->interface);
    if (![s->intf selectAlternateSetting:SL1_ALT_ACTIVE error:&e]) {
        set_err(err, errlen, "%s: set alt %d on interface %d: %s", s->info->name, SL1_ALT_ACTIVE,
                s->info->interface, nserr(e));
        return -1;
    }
    s->active = 1;
    trace("%s: copy pipe 0x%02x", s->info->name, s->info->endpoint);
    s->pipe = [s->intf copyPipeWithAddress:s->info->endpoint error:&e];
    if (!s->pipe) {
        set_err(err, errlen, "%s: pipe 0x%02x: %s", s->info->name, s->info->endpoint, nserr(e));
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
    NSUInteger buflen = (NSUInteger)s->info->max_packet * (NSUInteger)cfg->packets_per_transfer;
    for (int i = 0; i < s->nslots; i++) {
        xfer_slot *slot = &s->slots[i];
        slot->owner = s;
        slot->data = [s->intf ioDataWithCapacity:buflen error:&e];
        slot->tx = calloc((size_t)cfg->packets_per_transfer, sizeof(*slot->tx));
        if (!slot->data || slot->data.length < buflen || !slot->tx) {
            set_err(err, errlen, "%s: io buffer: %s", s->info->name, nserr(e));
            return -1;
        }
    }
    trace("%s: submit %d transfers", s->info->name, s->nslots);
    /* Submit on the I/O queue so next_frame is never touched concurrently. */
    __block int rc = 0;
    dispatch_sync(dev->queue, ^{
        s->next_frame = 0;
        for (int i = 0; i < s->nslots && rc == 0; i++) {
            if (submit(s, &s->slots[i]))
                rc = -1;
            else
                atomic_fetch_add(&s->in_flight, 1);
        }
    });
    if (rc)
        set_err(err, errlen, "%s: submit failed", s->info->name);
    trace("%s: streaming (rc %d)", s->info->name, rc);
    return rc;
}

int sl1_start(sl1_device *dev, unsigned mask, char *err, size_t errlen)
{
    atomic_store(&dev->running, 1);
    for (int i = 0; i < SL1_STREAM_COUNT; i++) {
        if (!(mask & SL1_STREAM_BIT(i)))
            continue;
        if (stream_start(&dev->streams[i], err, errlen)) {
            sl1_stop(dev);
            char why[256];
            snprintf(why, sizeof(why), "%s", err ? err : "start failed");
            return reset_device(dev, why, err, errlen);
        }
    }
    return 0;
}

void sl1_stop(sl1_device *dev)
{
    atomic_store(&dev->running, 0);
    trace("stop");
    for (int i = 0; i < SL1_STREAM_COUNT; i++) {
        stream *s = &dev->streams[i];
        if (s->active && s->pipe)
            [s->pipe abortWithOption:IOUSBHostAbortOptionSynchronous error:nil];
    }
    for (int tries = 0; tries < 100; tries++) {
        int busy = 0;
        for (int i = 0; i < SL1_STREAM_COUNT; i++)
            busy += atomic_load(&dev->streams[i].in_flight);
        if (!busy)
            break;
        usleep(10000);
    }
    /* Make sure no completion block is still running before freeing. */
    if (dev->queue)
        dispatch_sync(dev->queue, ^{});
    for (int i = 0; i < SL1_STREAM_COUNT; i++) {
        stream *s = &dev->streams[i];
        if (!s->active)
            continue;
        for (int k = 0; s->slots && k < s->nslots; k++) {
            s->slots[k].data = nil;
            free(s->slots[k].tx);
        }
        free(s->slots);
        free(s->scratch);
        s->slots = NULL;
        s->scratch = NULL;
        s->pipe = nil;
        trace("%s: select alt %d", s->info->name, SL1_ALT_IDLE);
        [s->intf selectAlternateSetting:SL1_ALT_IDLE error:nil];
        s->active = 0;
    }
}

void sl1_close(sl1_device *dev)
{
    if (!dev)
        return;
    sl1_stop(dev);
    for (int i = 0; i < SL1_STREAM_COUNT; i++) {
        stream *s = &dev->streams[i];
        [s->intf destroy];
        s->intf = nil;
        if (s->ring.data)
            sl1_ring_free(&s->ring);
        if (s->dev)
            pthread_mutex_destroy(&s->stats_lock);
    }
    trace("close");
    [dev->usbdev destroy];
    dev->usbdev = nil;
    dev->queue = nil;
    free(dev);
}

void sl1_set_callbacks(sl1_device *dev, const sl1_callbacks *cb)
{
    if (cb)
        dev->cb = *cb;
    else
        memset(&dev->cb, 0, sizeof(dev->cb));
}

int sl1_frame_time(sl1_device *dev, uint64_t *frame, uint64_t *host_time)
{
    for (int i = 0; i < SL1_STREAM_COUNT; i++) {
        IOUSBHostInterface *intf = dev->streams[i].intf;
        if (!intf)
            continue;
        IOUSBHostTime t = 0;
        uint64_t f = [intf frameNumberWithTime:&t];
        if (!f)
            return -1;
        *frame = f;
        *host_time = t;
        return 0;
    }
    return -1;
}

int sl1_location_id(sl1_device *dev, uint32_t *location)
{
    for (int i = 0; i < SL1_STREAM_COUNT; i++) {
        IOUSBHostInterface *intf = dev->streams[i].intf;
        if (!intf)
            continue;
        CFTypeRef v = IORegistryEntrySearchCFProperty(intf.ioService, kIOServicePlane, CFSTR("locationID"),
                                                      kCFAllocatorDefault,
                                                      kIORegistryIterateRecursively | kIORegistryIterateParents);
        if (!v)
            return -1;
        int ok = CFGetTypeID(v) == CFNumberGetTypeID() &&
                 CFNumberGetValue(v, kCFNumberSInt32Type, location);
        CFRelease(v);
        return ok ? 0 : -1;
    }
    return -1;
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
