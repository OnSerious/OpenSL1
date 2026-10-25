/* SPDX-License-Identifier: GPL-3.0-or-later */
/* OpenSL1 - helpers shared by the command line tools. */
#ifndef SL1_TOOLS_COMMON_H
#define SL1_TOOLS_COMMON_H

#include <stdio.h>
#include <string.h>

#include "../src/sl1_usb.h"

/* Maps a deck name to its IN/OUT stream. Returns 0 on success. */
static inline int parse_deck(const char *s, sl1_stream_id *in, sl1_stream_id *out)
{
    if (!strcmp(s, "A") || !strcmp(s, "a") || !strcmp(s, "1")) {
        *in = SL1_STREAM_A_IN;
        *out = SL1_STREAM_A_OUT;
        return 0;
    }
    if (!strcmp(s, "B") || !strcmp(s, "b") || !strcmp(s, "2")) {
        *in = SL1_STREAM_B_IN;
        *out = SL1_STREAM_B_OUT;
        return 0;
    }
    if (!strcmp(s, "mic") || !strcmp(s, "M") || !strcmp(s, "m")) {
        *in = SL1_STREAM_MIC_IN;
        *out = SL1_STREAM_COUNT; /* no output */
        return 0;
    }
    return -1;
}

static inline void print_stats(sl1_device *dev, sl1_stream_id id)
{
    sl1_stream_stats st;
    sl1_stream_get_stats(dev, id, &st);
    fprintf(stderr,
            "%-7s packets=%llu frames=%llu errors=%llu xrun=%llu resubmit_fail=%llu "
            "frames/packet=%u..%u\n",
            SL1_STREAMS[id].name, (unsigned long long)st.packets, (unsigned long long)st.frames,
            (unsigned long long)st.packet_errors, (unsigned long long)st.xrun,
            (unsigned long long)st.resubmit_fail, st.min_frames, st.max_frames);
}

#endif
