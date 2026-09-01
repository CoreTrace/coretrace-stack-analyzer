// SPDX-License-Identifier: Apache-2.0
#include <stdlib.h>

int pick_masked(int seed)
{
    int* table = (int*)malloc(8 * sizeof(int));
    if (!table)
        return 0;

    int index = seed & 7;
    int value = table[index];
    free(table);
    return value;
}

// `index` is masked to [0, 7] and the buffer holds 8 elements. No comparison exists in the
// IR, so the access can only be discharged by KnownBits reaching the load through the
// single-assignment slot that holds `index`. Before that path existed the analyzer reported
// a heap out-of-bounds read here.

// not contains: potential out-of-bounds read on heap buffer
