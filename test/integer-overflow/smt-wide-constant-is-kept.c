// SPDX-License-Identifier: Apache-2.0
//
// Guard: the SMT encoder must not narrow integer constants wider than 64 bits.
//
// A `__int128` constant does not fit the 64-bit value the constraint IR stores. Reading it with
// getSExtValue() aborts the analyzer when asserts are on, and otherwise keeps the low word only:
// `1 << 126` becomes 0, the query becomes `(x << 64) + 0`, and the solver drops a real overflow.
// A small negative constant widened to 128 bits must also stay negative. Both additions overflow
// for some input, so this fixture must report both, in BOTH passes.
//
// The values come from a `long` parameter: an `__int128` parameter is split in two registers on
// x86-64, and the default pass does not relate it to the parameter there.

__int128 add_high_bit(long x)
{
    return ((__int128)x << 64) + ((__int128)1 << 126);
}

__int128 high_word_minus_one(long x)
{
    return ((__int128)x << 64) + (-1);
}

// at line 16, column 32
// [ !!Warn ] potential signed integer overflow in arithmetic operation
// ↳ operation: add
// ↳ result is returned without a provable non-overflow bound

// at line 21, column 32
// [ !!Warn ] potential signed integer overflow in arithmetic operation
// ↳ operation: add
// ↳ result is returned without a provable non-overflow bound
