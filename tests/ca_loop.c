/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ca_loop - measures the SL1's round-trip latency through the installed
 * driver. Needs a cable from Line 1 (left) to Input 1 (left).
 *
 * Plays a short 1 kHz burst on Line 1 every 0.5 s and finds it again in
 * Input 1 with a matched filter. Both ends are compared in device sample
 * time, so the result is the delay the HAL does not know about yet (USB,
 * converters, input stage): the value kAudioDevicePropertyLatency should
 * report, summed over input and output.
 *
 * The SL1's inputs are phono level, so start quietly (default -50 dBFS).
 *
 *   ca_loop [-d seconds] [-t burst_dBFS] [-b buffer_frames]
 */
#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RATE 44100
#define PERIOD (RATE / 2)  /* one burst every 0.5 s */
#define BURST 132          /* 3 ms: three periods of 1 kHz */
#define SEARCH (RATE / 5)  /* look for the echo up to 200 ms after the burst */
#define MAX_BURSTS 256

static double amp;
static float burst[BURST];

/* Output: device sample time of every burst start. */
static double burst_time[MAX_BURSTS];
static _Atomic int bursts;
static double next_burst = -1;

/* Input 1 left, stored by device sample time relative to the first input sample. */
static float *rec;
static size_t rec_cap;
static double rec_t0 = -1;
static _Atomic unsigned long rec_frames;

static OSStatus ioproc(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in,
                       const AudioTimeStamp *in_time, AudioBufferList *out,
                       const AudioTimeStamp *out_time, void *ctx)
{
    (void)dev, (void)now, (void)ctx;
    if (in && in->mNumberBuffers && (in_time->mFlags & kAudioTimeStampSampleTimeValid)) {
        const float *x = in->mBuffers[0].mData;
        UInt32 ch = in->mBuffers[0].mNumberChannels;
        UInt32 n = in->mBuffers[0].mDataByteSize / (4 * ch);
        if (rec_t0 < 0)
            rec_t0 = in_time->mSampleTime;
        for (UInt32 i = 0; i < n; i++) {
            long pos = lround(in_time->mSampleTime - rec_t0) + (long)i;
            if (pos >= 0 && (size_t)pos < rec_cap)
                rec[pos] = x[i * ch];
        }
        rec_frames = (unsigned long)lround(in_time->mSampleTime - rec_t0) + n;
    }
    if (out && out->mNumberBuffers && (out_time->mFlags & kAudioTimeStampSampleTimeValid)) {
        float *y = out->mBuffers[0].mData;
        UInt32 ch = out->mBuffers[0].mNumberChannels;
        UInt32 n = out->mBuffers[0].mDataByteSize / (4 * ch);
        memset(y, 0, out->mBuffers[0].mDataByteSize);
        double t = out_time->mSampleTime;
        if (next_burst < 0)
            next_burst = t + RATE; /* skip the first second (the SL1's inputs settle) */
        for (UInt32 i = 0; i < n; i++) {
            double ti = t + i;
            double k = ti - next_burst;
            if (k >= 0 && k < BURST) {
                y[i * ch] = burst[(int)k]; /* Line 1 left only */
                if (k == 0 && bursts < MAX_BURSTS)
                    burst_time[bursts++] = ti;
            }
            if (k >= BURST - 1)
                next_burst += PERIOD;
        }
    }
    return noErr;
}

static AudioObjectID find_device(void)
{
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size))
        return 0;
    AudioObjectID *ids = malloc(size);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids);
    AudioObjectID found = 0;
    for (UInt32 i = 0; i < size / sizeof(AudioObjectID) && !found; i++) {
        CFStringRef uid = NULL;
        UInt32 s = sizeof(uid);
        AudioObjectPropertyAddress u = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(ids[i], &u, 0, NULL, &s, &uid) == noErr && uid) {
            if (CFStringHasPrefix(uid, CFSTR("OpenSL1:13e5:0001")))
                found = ids[i];
            CFRelease(uid);
        }
    }
    free(ids);
    return found;
}

static UInt32 get_u32(AudioObjectID dev, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress a = { sel, scope, kAudioObjectPropertyElementMain };
    UInt32 v = 0, size = sizeof(v);
    AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &v);
    return v;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    double seconds = 10, burst_db = -50;
    UInt32 buffer = 256;
    int opt;
    while ((opt = getopt(argc, argv, "d:t:b:")) != -1) {
        if (opt == 'd')
            seconds = atof(optarg);
        else if (opt == 't')
            burst_db = atof(optarg);
        else if (opt == 'b')
            buffer = (UInt32)atoi(optarg);
        else {
            fprintf(stderr, "usage: %s [-d seconds] [-t burst_dBFS] [-b buffer_frames]\n", argv[0]);
            return 2;
        }
    }
    amp = pow(10, burst_db / 20);
    for (int i = 0; i < BURST; i++) /* Hann-windowed 1 kHz */
        burst[i] = (float)(amp * sin(2 * M_PI * 1000.0 * i / RATE) * (0.5 - 0.5 * cos(2 * M_PI * i / (BURST - 1))));
    rec_cap = (size_t)((seconds + 2) * RATE);
    rec = calloc(rec_cap, sizeof(float));

    AudioObjectID dev = find_device();
    if (!dev) {
        fprintf(stderr, "Rane SL1 (OpenSL1) not found - driver installed and SL1 plugged in?\n");
        return 1;
    }
    AudioObjectPropertyAddress bs = { kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
                                      kAudioObjectPropertyElementMain };
    AudioObjectSetPropertyData(dev, &bs, 0, NULL, sizeof(buffer), &buffer);
    buffer = get_u32(dev, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal);
    UInt32 safety_in = get_u32(dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput);
    UInt32 safety_out = get_u32(dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput);
    UInt32 lat_in = get_u32(dev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput);
    UInt32 lat_out = get_u32(dev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput);

    AudioDeviceIOProcID proc;
    if (AudioDeviceCreateIOProcID(dev, ioproc, NULL, &proc) || AudioDeviceStart(dev, proc)) {
        fprintf(stderr, "cannot start IO\n");
        return 1;
    }
    usleep((useconds_t)(seconds * 1e6));
    AudioDeviceStop(dev, proc);
    AudioDeviceDestroyIOProcID(dev, proc);

    /* Matched filter: for each burst, the lag with the largest correlation. */
    double lags[MAX_BURSTS], noise = 0, signal = 0;
    int found = 0, n = bursts;
    unsigned long frames = rec_frames < rec_cap ? rec_frames : (unsigned long)rec_cap;
    for (int b = 0; b < n; b++) {
        long start = lround(burst_time[b] - rec_t0);
        if (start < 0 || (unsigned long)(start + SEARCH + BURST) > frames)
            continue;
        double best = 0, energy = 0;
        long best_lag = -1;
        for (long lag = 0; lag < SEARCH; lag++) {
            double c = 0;
            for (int i = 0; i < BURST; i++)
                c += rec[start + lag + i] * burst[i];
            if (fabs(c) > best) {
                best = fabs(c);
                best_lag = lag;
            }
            energy += c * c;
        }
        double mean = sqrt(energy / SEARCH);
        if (best_lag >= 0 && best > 8 * mean) {
            lags[found++] = (double)best_lag;
            signal += best;
            noise += mean;
        }
    }
    printf("buffer %u frames, safety offset in %u / out %u, reported latency in %u / out %u frames\n", buffer,
           safety_in, safety_out, lat_in, lat_out);
    printf("bursts sent %d, echoes found %d", n, found);
    if (!found) {
        printf("\nno echo - is Line 1 (left) cabled to Input 1 (left)? try a louder burst (-t -40)\n");
        return 1;
    }
    printf(" (peak/mean %.1f)\n", signal / noise);
    qsort(lags, (size_t)found, sizeof(double), cmp_double);
    double med = lags[found / 2];
    printf("loop delay not known to the HAL: median %.0f frames (%.2f ms), min %.0f, max %.0f\n", med,
           med * 1000.0 / RATE, lags[0], lags[found - 1]);
    double app = med + safety_in + safety_out + lat_in + lat_out + 2.0 * buffer;
    printf("round trip for an app with this buffer: about %.0f frames (%.1f ms)\n", app, app * 1000.0 / RATE);
    return 0;
}
