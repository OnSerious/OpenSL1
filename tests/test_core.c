/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests for the hardware-independent parts (no SL1 needed). */
#include <stdio.h>

#include "../src/sl1_pkt.h"
#include "../src/sl1_ring.h"

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static void test_pkt_clock(void)
{
    sl1_pkt_clock c;
    sl1_pkt_clock_init(&c, 44100, 1000);
    unsigned total = 0, n44 = 0, n45 = 0;
    for (int i = 0; i < 1000; i++) {
        unsigned f = sl1_pkt_clock_next(&c);
        CHECK(f == 44 || f == 45);
        n44 += f == 44;
        n45 += f == 45;
        total += f;
    }
    CHECK(total == 44100);
    CHECK(n45 == 100 && n44 == 900);
    /* 45-frame packets fit the 184-byte endpoint (45 * 4 = 180). */
    CHECK(45 * 4 <= 184);

    sl1_pkt_clock_init(&c, 32000, 1000);
    for (int i = 0; i < 10; i++)
        CHECK(sl1_pkt_clock_next(&c) == 32);
}

static void test_ring(void)
{
    sl1_ring r;
    CHECK(sl1_ring_init(&r, 100, 2) == 0);
    CHECK(r.capacity == 128);
    int16_t in[300 * 2], out[300 * 2];
    for (int i = 0; i < 600; i++)
        in[i] = (int16_t)i;

    CHECK(sl1_ring_write(&r, in, 100) == 100);
    CHECK(sl1_ring_available(&r) == 100);
    CHECK(sl1_ring_read(&r, out, 60) == 60);
    for (int i = 0; i < 120; i++)
        CHECK(out[i] == in[i]);
    /* Wraps around the end of the buffer. */
    CHECK(sl1_ring_write(&r, in + 200, 80) == 80);
    CHECK(sl1_ring_available(&r) == 120);
    CHECK(sl1_ring_read(&r, out, 120) == 120);
    for (int i = 0; i < 80; i++)
        CHECK(out[i] == in[120 + i]);
    for (int i = 0; i < 160; i++)
        CHECK(out[80 + i] == in[200 + i]);
    /* Full and empty limits. */
    CHECK(sl1_ring_write(&r, in, 300) == 128);
    CHECK(sl1_ring_space(&r) == 0);
    CHECK(sl1_ring_read(&r, out, 300) == 128);
    CHECK(sl1_ring_read(&r, out, 1) == 0);
    sl1_ring_free(&r);
}

int main(void)
{
    test_pkt_clock();
    test_ring();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
