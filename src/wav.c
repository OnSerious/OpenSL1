/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "wav.h"

#include <string.h>

static void put_u16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put_u32(unsigned char *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

static void make_header(unsigned char h[44], int channels, unsigned rate, uint32_t frames)
{
    uint32_t data_bytes = frames * (uint32_t)channels * 2;
    memcpy(h, "RIFF", 4);
    put_u32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_u32(h + 16, 16);
    put_u16(h + 20, 1); /* PCM */
    put_u16(h + 22, (uint16_t)channels);
    put_u32(h + 24, rate);
    put_u32(h + 28, rate * (uint32_t)channels * 2);
    put_u16(h + 32, (uint16_t)(channels * 2));
    put_u16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put_u32(h + 40, data_bytes);
}

int wav_open(wav_writer *w, const char *path, int channels, unsigned rate)
{
    unsigned char h[44];
    w->f = fopen(path, "wb");
    if (!w->f)
        return -1;
    w->channels = channels;
    w->rate = rate;
    w->frames = 0;
    make_header(h, channels, rate, 0);
    return fwrite(h, 1, sizeof(h), w->f) == sizeof(h) ? 0 : -1;
}

int wav_write(wav_writer *w, const int16_t *frames, uint32_t count)
{
    /* Samples are written as little-endian; both supported hosts are little-endian. */
    size_t n = fwrite(frames, (size_t)w->channels * 2, count, w->f);
    w->frames += (uint32_t)n;
    return n == count ? 0 : -1;
}

int wav_close(wav_writer *w)
{
    unsigned char h[44];
    make_header(h, w->channels, w->rate, w->frames);
    int rc = fseek(w->f, 0, SEEK_SET) == 0 && fwrite(h, 1, sizeof(h), w->f) == sizeof(h) ? 0 : -1;
    if (fclose(w->f))
        rc = -1;
    w->f = NULL;
    return rc;
}
