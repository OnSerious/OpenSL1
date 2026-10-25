/* SPDX-License-Identifier: GPL-3.0-or-later */
/* OpenSL1 - minimal 16-bit PCM WAV writer. */
#ifndef SL1_WAV_H
#define SL1_WAV_H

#include <stdint.h>
#include <stdio.h>

typedef struct {
    FILE *f;
    int channels;
    unsigned rate;
    uint32_t frames;
} wav_writer;

int wav_open(wav_writer *w, const char *path, int channels, unsigned rate);
int wav_write(wav_writer *w, const int16_t *frames, uint32_t count);
int wav_close(wav_writer *w); /* patches the header sizes */

#endif
