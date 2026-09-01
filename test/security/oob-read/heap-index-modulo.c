// SPDX-License-Identifier: Apache-2.0
#include <stdlib.h>

int pick_modulo(unsigned seed)
{
    int* table = (int*)malloc(16 * sizeof(int));
    if (!table)
        return 0;

    unsigned index = seed % 16u;
    int value = table[index];
    free(table);
    return value;
}

// Same shape as heap-index-masked, through `urem` instead of `and`: KnownBits bounds
// `seed % 16` to [0, 15] for a 16-element buffer.

// not contains: potential out-of-bounds read on heap buffer
