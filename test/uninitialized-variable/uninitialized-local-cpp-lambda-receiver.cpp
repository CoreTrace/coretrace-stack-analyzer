// SPDX-License-Identifier: Apache-2.0
#include <limits.h>

int lambda_receiver_object_should_not_warn_never_initialized(void)
{
    auto buildCanonicalize = [&](int v) { return v + 1; };
    auto buildCanonicalize2 = [&](int v)
    {
        if (v >= INT_MIN && v < INT_MAX)
            return v; // ou autre politique
        return v + 1;
    };

    return buildCanonicalize(41);
}

// not contains: local variable 'buildCanonicalize' is never initialized

// at line 5, column 52
// [ !!Warn ] potential signed integer overflow in arithmetic operation
//          ↳ operation: add
//          ↳ result is returned without a provable non-overflow boun
