/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sl1tone - play a sine tone on an SL1 line output.
 *
 *   sl1tone [-o A|B] [-f hz] [-l dBFS] [-d seconds]
 */
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int main(int argc, char **argv)
{
    const char *deck = "A";
    double freq = 440, level_db = -12, seconds = 5;
    int opt;
    while ((opt = getopt(argc, argv, "o:f:l:d:h")) != -1) {
        switch (opt) {
        case 'o': deck = optarg; break;
        case 'f': freq = atof(optarg); break;
        case 'l': level_db = atof(optarg); break;
        case 'd': seconds = atof(optarg); break;
        default:
            fprintf(stderr, "usage: %s [-o A|B] [-f hz] [-l dBFS] [-d seconds]\n", argv[0]);
            return 2;
        }
    }
    sl1_stream_id in, out;
    if (parse_deck(deck, &in, &out) || out == SL1_STREAM_COUNT) {
        fprintf(stderr, "unknown output '%s' (A or B)\n", deck);
        return 2;
    }
    if (level_db > 0)
        level_db = 0;

    char err[256];
    sl1_device *dev;
    if (sl1_open(&dev, NULL, err, sizeof(err))) {
        fprintf(stderr, "sl1tone: %s\n", err);
        return 1;
    }
    sl1_ring *ring = sl1_stream_ring(dev, out);

    /* Prefill 50 ms so the first transfers carry audio. */
    double amp = 32767 * pow(10, level_db / 20), phase = 0, step = 2 * M_PI * freq / SL1_RATE;
    uint64_t total = (uint64_t)(seconds * SL1_RATE), done = 0;
    unsigned fade = SL1_RATE / 20;
    int16_t buf[512 * 2];
    int started = 0;
    struct timespec nap = { 0, 2000000 };
    while (done < total) {
        size_t n = sl1_ring_space(ring);
        if (n > 512)
            n = 512;
        if (n > total - done)
            n = (size_t)(total - done);
        if (!n) {
            nanosleep(&nap, NULL);
            continue;
        }
        for (size_t i = 0; i < n; i++, done++) {
            double g = 1;
            if (done < fade)
                g = (double)done / fade;
            if (total - done < fade)
                g = (double)(total - done) / fade;
            int16_t v = (int16_t)lrint(amp * g * sin(phase));
            phase += step;
            buf[2 * i] = buf[2 * i + 1] = v;
        }
        sl1_ring_write(ring, buf, n);
        if (!started && done >= SL1_RATE / 20) {
            if (sl1_start(dev, SL1_STREAM_BIT(out), err, sizeof(err))) {
                fprintf(stderr, "sl1tone: %s\n", err);
                sl1_close(dev);
                return 1;
            }
            started = 1;
            fprintf(stderr, "playing %.0f Hz at %.1f dBFS on %s for %.1f s\n", freq, level_db,
                    SL1_STREAMS[out].name, seconds);
        }
    }
    if (!started && sl1_start(dev, SL1_STREAM_BIT(out), err, sizeof(err)) == 0)
        started = 1;
    /* Wait until the ring has drained, then a little more for in-flight packets. */
    while (started && sl1_ring_available(ring))
        nanosleep(&nap, NULL);
    sl1_stream_stats during;
    sl1_stream_get_stats(dev, out, &during);
    struct timespec tail = { 0, 100000000 };
    nanosleep(&tail, NULL);
    sl1_stop(dev);
    print_stats(dev, out);
    /* Packets sent while the tone was still playing that found the ring empty. */
    fprintf(stderr, "underruns while playing: %llu (later ones are the silent tail)\n",
            (unsigned long long)during.xrun);
    sl1_close(dev);
    return 0;
}
