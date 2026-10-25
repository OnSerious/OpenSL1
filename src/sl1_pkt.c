/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sl1_pkt.h"

void sl1_pkt_clock_init(sl1_pkt_clock *c, unsigned rate, unsigned packets_per_second)
{
    c->rate = rate;
    c->packets_per_second = packets_per_second;
    c->remainder = 0;
}

unsigned sl1_pkt_clock_next(sl1_pkt_clock *c)
{
    unsigned base = c->rate / c->packets_per_second;
    c->remainder += c->rate % c->packets_per_second;
    if (c->remainder >= c->packets_per_second) {
        c->remainder -= c->packets_per_second;
        return base + 1;
    }
    return base;
}
