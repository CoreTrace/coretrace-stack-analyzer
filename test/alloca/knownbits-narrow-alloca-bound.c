// SPDX-License-Identifier: Apache-2.0
//
// Pins the exact bound KnownBits derives for a VLA size, and pins it *strictly*.
//
// `scale` is a uint8_t, so `scale * 512` is at most 255 * 512 = 130560. No comparison
// appears anywhere in the IR, so only KnownBits (via ValueTracking) can establish this.
//
// strict-expectation-details is on because the value 130560 lives in a "↳" detail line, and
// the default expectation matcher falls back to comparing diagnostic headlines only -- under
// which this number could drift to anything without failing.

// strict-expectation-details: true

#include <stdint.h>

char reserve_scaled(uint8_t scale)
{
    char buf[(unsigned long)scale * 512UL];
    buf[0] = 0;
    return buf[0];
}

// at line 18, column 5
// [ !!Warn ] dynamic stack allocation detected for variable 'vla'
// ↳ allocated type: i8
// ↳ size of this allocation is not compile-time constant (VLA / variable alloca) and may lead to unbounded stack usage

// at line 18, column 5
// [ !!Warn ] user-controlled alloca size for variable 'vla'
// ↳ allocation performed via alloca/VLA; stack usage grows with runtime value
// ↳ inferred upper bound for size: 130560 bytes
// ↳ size depends on user-controlled input (function argument or non-local value)
