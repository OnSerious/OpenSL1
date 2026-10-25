/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sl1rec - record one SL1 input to a WAV file and report level and
 * packet statistics.
 *
 *   sl1rec [-i A|B|mic] [-d seconds] [-o file.wav]
 */
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "../src/wav.h"
#include "common.h"

int main(int argc, char **argv)
{
    const char *deck = "A", *path = "sl1rec.wav";
    double seconds = 5;
    int opt;
    while ((opt = getopt(argc, argv, "i:d:o:h")) != -1) {
        switch (opt) {
        case 'i': deck = optarg; break;
        case 'd': seconds = atof(optarg); break;
        case 'o': path = optarg; break;
        default:
            fprintf(stderr, "usage: %s [-i A|B|mic] [-d seconds] [-o file.wav]\n", argv[0]);
            return 2;
        }
    }
    sl1_stream_id in, out;
    if (parse_deck(deck, &in, &out)) {
        fprintf(stderr, "unknown input '%s'\n", deck);
        return 2;
    }
    int ch = SL1_STREAMS[in].channels;

    char err[256];
    sl1_device *dev;
    if (sl1_open(&dev, NULL, err, sizeof(err))) {
        fprintf(stderr, "sl1rec: %s\n", err);
        return 1;
    }
    wav_writer w;
    if (wav_open(&w, path, ch, SL1_RATE)) {
        fprintf(stderr, "sl1rec: cannot write %s\n", path);
        sl1_close(dev);
        return 1;
    }
    if (sl1_start(dev, SL1_STREAM_BIT(in), err, sizeof(err))) {
        fprintf(stderr, "sl1rec: %s\n", err);
        wav_close(&w);
        sl1_close(dev);
        return 1;
    }

    uint32_t target = (uint32_t)(seconds * SL1_RATE);
    int16_t buf[1024 * 2];
    double sum[2] = { 0, 0 };
    int peak[2] = { 0, 0 };
    sl1_ring *ring = sl1_stream_ring(dev, in);
    struct timespec nap = { 0, 2000000 };
    while (w.frames < target) {
        size_t want = target - w.frames;
        if (want > 1024)
            want = 1024;
        size_t got = sl1_ring_read(ring, buf, want);
        if (!got) {
            nanosleep(&nap, NULL);
            continue;
        }
        for (size_t f = 0; f < got; f++)
            for (int c = 0; c < ch; c++) {
                int v = buf[f * (size_t)ch + (size_t)c];
                sum[c] += (double)v * v;
                if (abs(v) > peak[c])
                    peak[c] = abs(v);
            }
        wav_write(&w, buf, (uint32_t)got);
    }
    sl1_stop(dev);
    print_stats(dev, in);
    for (int c = 0; c < ch; c++) {
        double rms = sqrt(sum[c] / (w.frames ? w.frames : 1));
        fprintf(stderr, "channel %d: rms %.1f dBFS, peak %.1f dBFS\n", c,
                20 * log10(fmax(rms, 1e-9) / 32768), 20 * log10(fmax(peak[c], 1) / 32768.0));
    }
    fprintf(stderr, "wrote %u frames to %s\n", w.frames, path);
    wav_close(&w);
    sl1_close(dev);
    return 0;
}
