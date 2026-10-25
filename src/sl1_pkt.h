/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * OpenSL1 - packet sizing for 1 ms full-speed isochronous frames.
 *
 * 44100 Hz does not divide into 1 ms packets, so packets carry 44 or 45
 * frames. The accumulator keeps the long-term average exact.
 */
#ifndef SL1_PKT_H
#define SL1_PKT_H

typedef struct {
    unsigned rate;      /* frames per second */
    unsigned packets_per_second;
    unsigned remainder; /* accumulated fraction, in units of 1/packets_per_second frames */
} sl1_pkt_clock;

void sl1_pkt_clock_init(sl1_pkt_clock *c, unsigned rate, unsigned packets_per_second);

/* Number of frames for the next packet. */
unsigned sl1_pkt_clock_next(sl1_pkt_clock *c);

#endif
