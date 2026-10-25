/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * OpenSL1 - Rane SL1 (Serato Scratch Live) USB interface constants.
 *
 * All values come from our own measurements of the device (lsusb -v,
 * ALSA stream info, capture/playback tests), see docs/PROTOCOL.md.
 */
#ifndef SL1_PROTO_H
#define SL1_PROTO_H

#define SL1_VID 0x13e5
#define SL1_PID 0x0001

#define SL1_CONFIGURATION 1
#define SL1_NUM_INTERFACES 9

/* Streaming interfaces are idle on alt 0 and active on alt 1. */
#define SL1_ALT_IDLE 0
#define SL1_ALT_ACTIVE 1

#define SL1_RATE 44100
#define SL1_BYTES_PER_SAMPLE 2 /* S16_LE */

typedef enum {
    SL1_STREAM_A_OUT = 0, /* Line 1 */
    SL1_STREAM_A_IN,      /* Input 1 */
    SL1_STREAM_B_OUT,     /* Line 2 */
    SL1_STREAM_B_IN,      /* Input 2 */
    SL1_STREAM_MIC_IN,    /* Mic (mono) */
    SL1_STREAM_COUNT
} sl1_stream_id;

typedef struct {
    const char *name;
    int interface;
    unsigned char endpoint;
    int channels;
    int max_packet; /* wMaxPacketSize in bytes */
    int is_input;
} sl1_stream_info;

static const sl1_stream_info SL1_STREAMS[SL1_STREAM_COUNT] = {
    [SL1_STREAM_A_OUT]  = { "A-out",  1, 0x01, 2, 184, 0 },
    [SL1_STREAM_A_IN]   = { "A-in",   2, 0x82, 2, 184, 1 },
    [SL1_STREAM_B_OUT]  = { "B-out",  4, 0x04, 2, 184, 0 },
    [SL1_STREAM_B_IN]   = { "B-in",   5, 0x84, 2, 184, 1 },
    [SL1_STREAM_MIC_IN] = { "mic-in", 7, 0x85, 1, 144, 1 },
};

#define SL1_STREAM_BIT(id) (1u << (id))

#endif
