/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * hal_harness - loads OpenSL1.driver in-process and drives it like the
 * CoreAudio HAL would, so the driver can be tested without installing it.
 *
 *   hal_harness [-d seconds] [-t tone_dBFS] [path/to/OpenSL1.driver]
 *
 * Checks: device appears, properties answer, IO starts, zero timestamps
 * advance at ~44.1 kHz with little jitter, input levels; plays a 440 Hz tone
 * on all four outputs.
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CH 4
#define BUF 512

typedef void *(*factory_fn)(CFAllocatorRef, CFUUIDRef);

static OSStatus host_changed(AudioServerPlugInHostRef h, AudioObjectID obj, UInt32 n,
                             const AudioObjectPropertyAddress *a)
{
    (void)h;
    for (UInt32 i = 0; i < n; i++)
        printf("  [host] property changed: object %u '%.4s'\n", obj, (const char *)&(UInt32){
                                                                         CFSwapInt32HostToBig(a[i].mSelector) });
    return noErr;
}

static OSStatus host_copy(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef *d)
{
    (void)h, (void)k;
    *d = NULL;
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus host_write(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef d)
{
    (void)h, (void)k, (void)d;
    return noErr;
}

static OSStatus host_delete(AudioServerPlugInHostRef h, CFStringRef k)
{
    (void)h, (void)k;
    return noErr;
}

static OSStatus host_config(AudioServerPlugInHostRef h, AudioObjectID dev, UInt64 action, void *info)
{
    (void)h, (void)dev, (void)action, (void)info;
    return noErr;
}

static AudioServerPlugInHostInterface host_if = { host_changed, host_copy, host_write, host_delete,
                                                  host_config };

static AudioServerPlugInDriverRef drv;

static OSStatus get(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope,
                    UInt32 elem, void *out, UInt32 cap, UInt32 *size)
{
    AudioObjectPropertyAddress a = { sel, scope, elem };
    if (!(*drv)->HasProperty(drv, obj, 0, &a))
        return kAudioHardwareUnknownPropertyError;
    return (*drv)->GetPropertyData(drv, obj, 0, &a, 0, NULL, cap, size, out);
}

static void print_string(AudioObjectID obj, AudioObjectPropertySelector sel, const char *label,
                         AudioObjectPropertyScope scope, UInt32 elem)
{
    CFStringRef s = NULL;
    UInt32 size = 0;
    char buf[256] = "?";
    if (get(obj, sel, scope, elem, &s, sizeof(s), &size) == noErr && s) {
        CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8);
        CFRelease(s);
    }
    printf("  %-22s %s\n", label, buf);
}

static UInt32 get_u32(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope)
{
    UInt32 v = 0xffffffff, size = 0;
    get(obj, sel, scope, 0, &v, sizeof(v), &size);
    return v;
}

int main(int argc, char **argv)
{
    double seconds = 10, tone_db = -20;
    int opt;
    while ((opt = getopt(argc, argv, "d:t:")) != -1) {
        if (opt == 'd')
            seconds = atof(optarg);
        else if (opt == 't')
            tone_db = atof(optarg);
        else {
            fprintf(stderr, "usage: %s [-d seconds] [-t tone_dBFS] [OpenSL1.driver]\n", argv[0]);
            return 2;
        }
    }
    const char *bundle = optind < argc ? argv[optind] : "build/OpenSL1.driver";
    char path[1024];
    snprintf(path, sizeof(path), "%s/Contents/MacOS/OpenSL1", bundle);
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    factory_fn create = (factory_fn)dlsym(lib, "OpenSL1_Create");
    if (!create) {
        fprintf(stderr, "factory OpenSL1_Create not found\n");
        return 1;
    }
    drv = create(NULL, kAudioServerPlugInTypeUUID);
    if (!drv || (*drv)->Initialize(drv, &host_if)) {
        fprintf(stderr, "driver did not initialize\n");
        return 1;
    }

    AudioObjectID devs[4];
    UInt32 size = 0;
    get(kAudioObjectPlugInObject, kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, 0,
        devs, sizeof(devs), &size);
    UInt32 ndev = size / sizeof(AudioObjectID);
    printf("devices: %u\n", ndev);
    if (!ndev) {
        fprintf(stderr, "no device (is the SL1 plugged in?)\n");
        return 1;
    }
    AudioObjectID dev = devs[0];
    print_string(dev, kAudioObjectPropertyName, "name", kAudioObjectPropertyScopeGlobal, 0);
    print_string(dev, kAudioObjectPropertyManufacturer, "manufacturer", kAudioObjectPropertyScopeGlobal, 0);
    print_string(dev, kAudioDevicePropertyDeviceUID, "device UID", kAudioObjectPropertyScopeGlobal, 0);
    print_string(dev, kAudioDevicePropertyModelUID, "model UID", kAudioObjectPropertyScopeGlobal, 0);
    for (UInt32 c = 1; c <= CH; c++) {
        print_string(dev, kAudioObjectPropertyElementName, "input channel", kAudioObjectPropertyScopeInput, c);
        print_string(dev, kAudioObjectPropertyElementName, "output channel", kAudioObjectPropertyScopeOutput, c);
    }
    UInt32 safety_in = get_u32(dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput);
    UInt32 safety_out = get_u32(dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput);
    UInt32 period = get_u32(dev, kAudioDevicePropertyZeroTimeStampPeriod, kAudioObjectPropertyScopeGlobal);
    printf("  %-22s in %u, out %u frames; zero timestamp period %u\n", "safety offset", safety_in,
           safety_out, period);
    AudioObjectID streams[4];
    get(dev, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeGlobal, 0, streams, sizeof(streams), &size);
    if (size != 2 * sizeof(AudioObjectID)) {
        fprintf(stderr, "expected 2 streams\n");
        return 1;
    }
    AudioObjectID s_in = streams[0], s_out = streams[1];
    AudioStreamBasicDescription f;
    get(s_in, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, 0, &f, sizeof(f), &size);
    printf("  %-22s %.0f Hz, %u ch, %u bit\n", "format", f.mSampleRate, f.mChannelsPerFrame, f.mBitsPerChannel);

    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    double ticks_per_sec = 1e9 * tb.denom / tb.numer;

    uint64_t t0 = mach_absolute_time();
    OSStatus r = (*drv)->StartIO(drv, dev, 1);
    printf("StartIO: %d (%.0f ms)\n", (int)r, (mach_absolute_time() - t0) / ticks_per_sec * 1e3);
    if (r)
        return 1;

    float *in = calloc(BUF * CH, sizeof(float)), *out = calloc(BUF * CH, sizeof(float));
    double sum[CH] = { 0 }, peak[CH] = { 0 };
    uint64_t in_frames = 0;
    double amp = pow(10, tone_db / 20), phase = 0;
    Float64 zs, first_zs = -1, last_zs = -1;
    UInt64 zh, first_zh = 0, last_zh = 0, seed;
    unsigned zero_changes = 0, cycles = 0, errors = 0;
    double max_step_err_us = 0;
    double next_cycle_sample = -1;

    uint64_t end = mach_absolute_time() + (uint64_t)(seconds * ticks_per_sec);
    while (mach_absolute_time() < end) {
        if ((*drv)->GetZeroTimeStamp(drv, dev, 1, &zs, &zh, &seed)) {
            errors++;
            usleep(2000);
            continue;
        }
        if (zs != last_zs) {
            if (last_zs >= 0) {
                /* Each step should be exactly one period at 44.1 kHz. */
                double expect = (zs - last_zs) / 44100.0 * ticks_per_sec;
                double err_us = fabs((double)(zh - last_zh) - expect) / ticks_per_sec * 1e6;
                if (err_us > max_step_err_us)
                    max_step_err_us = err_us;
                zero_changes++;
            } else {
                first_zs = zs;
                first_zh = zh;
            }
            last_zs = zs;
            last_zh = zh;
        }
        double now_sample = zs + ((double)mach_absolute_time() - (double)zh) / ticks_per_sec * 44100.0;
        if (next_cycle_sample < 0)
            next_cycle_sample = floor(now_sample);
        if (now_sample < next_cycle_sample + BUF) {
            usleep(1000);
            continue;
        }
        AudioServerPlugInIOCycleInfo ci;
        memset(&ci, 0, sizeof(ci));
        ci.mIOCycleCounter = cycles;
        ci.mNominalIOBufferFrameSize = BUF;
        ci.mInputTime.mSampleTime = next_cycle_sample - safety_in;
        ci.mOutputTime.mSampleTime = next_cycle_sample + BUF + safety_out;
        (*drv)->DoIOOperation(drv, dev, s_in, 1, kAudioServerPlugInIOOperationReadInput, BUF, &ci, in, NULL);
        for (int i = 0; i < BUF; i++) {
            float v = (float)(amp * sin(phase));
            phase += 2 * M_PI * 440 / 44100;
            for (int c = 0; c < CH; c++) {
                out[i * CH + c] = v;
                double x = in[i * CH + c];
                sum[c] += x * x;
                if (fabs(x) > peak[c])
                    peak[c] = fabs(x);
            }
        }
        in_frames += BUF;
        (*drv)->DoIOOperation(drv, dev, s_out, 1, kAudioServerPlugInIOOperationWriteMix, BUF, &ci, out, NULL);
        next_cycle_sample += BUF;
        cycles++;
    }
    (*drv)->StopIO(drv, dev, 1);

    double rate = last_zh > first_zh ? (last_zs - first_zs) / ((double)(last_zh - first_zh) / ticks_per_sec) : 0;
    printf("IO cycles: %u (%u frames each), zero timestamp errors: %u\n", cycles, BUF, errors);
    printf("zero timestamps: %u periods, rate %.2f Hz, max step error %.1f us\n", zero_changes, rate,
           max_step_err_us);
    for (int c = 0; c < CH; c++) {
        double rms = sqrt(sum[c] / (in_frames ? in_frames : 1));
        printf("input ch %d: rms %.1f dBFS, peak %.1f dBFS\n", c + 1, 20 * log10(fmax(rms, 1e-9)),
               20 * log10(fmax(peak[c], 1e-9)));
    }
    return 0;
}
