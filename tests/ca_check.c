/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ca_check - checks the installed OpenSL1 driver through CoreAudio, the way
 * an app like Mixxx uses it: opens "Rane SL1", runs an IOProc, measures input
 * levels and plays a 440 Hz tone on all outputs.
 *
 *   ca_check [-d seconds] [-t tone_dBFS] [-b buffer_frames] [-r raw_input.f32]
 */
#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CH 4

static double amp, phase;
static double sum[CH], peak[CH];
static _Atomic unsigned long callbacks, in_frames, out_frames;
static _Atomic unsigned overloads;
static float *rec;
static size_t rec_cap;

static OSStatus ioproc(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in,
                       const AudioTimeStamp *in_time, AudioBufferList *out,
                       const AudioTimeStamp *out_time, void *ctx)
{
    (void)dev, (void)now, (void)in_time, (void)out_time, (void)ctx;
    if (in && in->mNumberBuffers) {
        const float *x = in->mBuffers[0].mData;
        UInt32 ch = in->mBuffers[0].mNumberChannels;
        UInt32 n = in->mBuffers[0].mDataByteSize / (4 * ch);
        for (UInt32 i = 0; i < n; i++)
            for (UInt32 c = 0; c < ch && c < CH; c++) {
                double v = x[i * ch + c];
                sum[c] += v * v;
                if (fabs(v) > peak[c])
                    peak[c] = fabs(v);
            }
        if (rec && ch == CH)
            for (UInt32 i = 0; i < n && in_frames + i < rec_cap; i++)
                memcpy(&rec[(in_frames + i) * CH], &x[i * ch], CH * sizeof(float));
        in_frames += n;
    }
    if (out && out->mNumberBuffers) {
        float *y = out->mBuffers[0].mData;
        UInt32 ch = out->mBuffers[0].mNumberChannels;
        UInt32 n = out->mBuffers[0].mDataByteSize / (4 * ch);
        for (UInt32 i = 0; i < n; i++) {
            float v = (float)(amp * sin(phase));
            phase += 2 * M_PI * 440 / 44100;
            for (UInt32 c = 0; c < ch; c++)
                y[i * ch + c] = v;
        }
        out_frames += n;
    }
    callbacks++;
    return noErr;
}

static OSStatus on_overload(AudioObjectID obj, UInt32 n, const AudioObjectPropertyAddress *a, void *ctx)
{
    (void)obj, (void)n, (void)a, (void)ctx;
    overloads++;
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

int main(int argc, char **argv)
{
    double seconds = 6, tone_db = -20;
    UInt32 buffer = 512;
    const char *raw = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "d:t:b:r:")) != -1) {
        if (opt == 'd')
            seconds = atof(optarg);
        else if (opt == 't')
            tone_db = atof(optarg);
        else if (opt == 'b')
            buffer = (UInt32)atoi(optarg);
        else if (opt == 'r')
            raw = optarg;
        else {
            fprintf(stderr, "usage: %s [-d seconds] [-t tone_dBFS] [-b buffer_frames] [-r raw_input.f32]\n", argv[0]);
            return 2;
        }
    }
    amp = pow(10, tone_db / 20);
    if (raw) {
        rec_cap = (size_t)((seconds + 1) * 44100);
        rec = calloc(rec_cap * CH, sizeof(float));
    }
    AudioObjectID dev = find_device();
    if (!dev) {
        fprintf(stderr, "Rane SL1 (OpenSL1) not found - driver installed and SL1 plugged in?\n");
        return 1;
    }
    AudioObjectPropertyAddress bs = { kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
                                      kAudioObjectPropertyElementMain };
    AudioObjectSetPropertyData(dev, &bs, 0, NULL, sizeof(buffer), &buffer);
    UInt32 size = sizeof(buffer);
    AudioObjectGetPropertyData(dev, &bs, 0, NULL, &size, &buffer);
    UInt32 lat[2] = { 0, 0 };
    for (int s = 0; s < 2; s++) {
        AudioObjectPropertyScope scope = s ? kAudioObjectPropertyScopeOutput : kAudioObjectPropertyScopeInput;
        UInt32 v = 0;
        AudioObjectPropertyAddress la = { kAudioDevicePropertyLatency, scope, kAudioObjectPropertyElementMain };
        AudioObjectPropertyAddress sa = { kAudioDevicePropertySafetyOffset, scope, kAudioObjectPropertyElementMain };
        size = sizeof(v);
        AudioObjectGetPropertyData(dev, &la, 0, NULL, &size, &v);
        lat[s] += v;
        size = sizeof(v);
        AudioObjectGetPropertyData(dev, &sa, 0, NULL, &size, &v);
        lat[s] += v + buffer;
    }
    printf("device %u, buffer %u frames; latency in %.1f ms, out %.1f ms (buffer + safety offset)\n", dev,
           buffer, lat[0] / 44.1, lat[1] / 44.1);

    AudioObjectPropertyAddress ov = { kAudioDeviceProcessorOverload, kAudioObjectPropertyScopeGlobal,
                                      kAudioObjectPropertyElementMain };
    AudioObjectAddPropertyListener(dev, &ov, on_overload, NULL);
    AudioDeviceIOProcID proc;
    if (AudioDeviceCreateIOProcID(dev, ioproc, NULL, &proc) || AudioDeviceStart(dev, proc)) {
        fprintf(stderr, "cannot start IO\n");
        return 1;
    }
    usleep((useconds_t)(seconds * 1e6));
    AudioDeviceStop(dev, proc);
    AudioDeviceDestroyIOProcID(dev, proc);
    AudioObjectRemovePropertyListener(dev, &ov, on_overload, NULL);

    if (raw) {
        FILE *f = fopen(raw, "wb");
        size_t n = in_frames < rec_cap ? in_frames : rec_cap;
        if (f) {
            fwrite(rec, sizeof(float) * CH, n, f);
            fclose(f);
        }
    }
    double expect = seconds * 44100;
    printf("callbacks %lu, input frames %lu, output frames %lu (expected ~%.0f), overloads %u\n",
           (unsigned long)callbacks, (unsigned long)in_frames, (unsigned long)out_frames, expect,
           (unsigned)overloads);
    for (int c = 0; c < CH; c++) {
        double rms = sqrt(sum[c] / (in_frames ? in_frames : 1));
        printf("input ch %d: rms %.1f dBFS, peak %.1f dBFS\n", c + 1, 20 * log10(fmax(rms, 1e-9)),
               20 * log10(fmax(peak[c], 1e-9)));
    }
    return 0;
}
