// SPDX-License-Identifier: Apache-2.0
//
// Guards IntRanges.cpp::trivialRange() / informativeBounds().
//
// The VLA size is `zext i32 %count to i64`, so KnownBits proves it is at most 4294967295.
// That is true for every possible `count`: it restates the width of the source type and
// bounds nothing. Publishing it as an upper bound flips the diagnostic from the actionable
// "unbounded / user-controlled" form to "large alloca, inferred upper bound 4294967295",
// which is strictly less useful and names a 4 GiB "bound" that is not one.

unsigned char first_byte(unsigned count)
{
    char buf[count];
    buf[0] = 0;
    return (unsigned char)buf[0];
}

// strict-expectation-details: true

// at line 13, column 5
// [ !!Warn ] dynamic stack allocation detected for variable 'vla'
// ↳ allocated type: i8
// ↳ size of this allocation is not compile-time constant (VLA / variable alloca) and may lead to unbounded stack usage

// at line 13, column 5
// [ !!Warn ] user-controlled alloca size for variable 'vla'
// ↳ allocation performed via alloca/VLA; stack usage grows with runtime value
// ↳ size is unbounded at compile time
// ↳ size depends on user-controlled input (function argument or non-local value)

// not contains: inferred upper bound for size: 4294967295 bytes
