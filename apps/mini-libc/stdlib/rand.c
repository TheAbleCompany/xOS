// SPDX-License-Identifier: BSD-3-Clause

#include <stdlib.h>

static unsigned int ax_rng_state = 1u;

void srand(unsigned int seed)
{
        ax_rng_state = seed ? seed : 1u;
}

int rand(void)
{
        unsigned int x = ax_rng_state;

        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;

        ax_rng_state = x;

        return (int)(x & 0x7fffffffu);
}
