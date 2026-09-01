// SPDX-License-Identifier: Apache-2.0
//
// KNOWN LIMITATION -- this fixture pins behaviour we would like to change.
//
// FunctionFacts owns an AssumptionCache, so llvm.assume is available to LazyValueInfo and
// KnownBits. It still buys nothing here: at -O0 the assumption constrains the SSA load that
// __builtin_assume was applied to, while the indexed access reads the slot through a second,
// distinct load. Assumptions do not transfer between two loads of the same alloca, so the
// index stays unbounded and the access is reported even though it is provably in range.
//
// When assumption propagation across a slot lands, this fixture must be inverted: replace
// the expectation below with a "// not contains:" line.
#include <stdlib.h>

int pick_assumed(int n)
{
    int* table = (int*)malloc(16 * sizeof(int));
    if (!table)
        return 0;

    __builtin_assume(n >= 0 && n < 16);

    int value = table[n];
    free(table);
    return value;
}

// strict-expectation-details: true

// at line 23, column 17
// [ !!Warn ] potential out-of-bounds read on heap buffer 'call' via unchecked index
// ↳ inferred heap capacity: 16 element(s)
// ↳ index value is not proven to be within [0, capacity-1]
