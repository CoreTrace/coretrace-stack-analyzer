// SPDX-License-Identifier: Apache-2.0
// A comparison against a constant gives a range bound only when the bound fits in 64 bits.
// A constant wider than 64 bits used to abort assert-enabled builds (getSExtValue) and to be
// truncated to its low 64 bits in release builds; C - 1 used to wrap past LONG_MIN.

#define TWO_TO_THE_64 ((__int128)1 << 64)
#define UNSIGNED_TWO_TO_THE_64 ((unsigned __int128)1 << 64)

extern __int128 wide_input(void);

// Never runs: no x is both >= 0 and < -2^64 + 20. Truncated, the constant became 20 and the
// access was reported as going up to 19.
char below_minus_two_to_the_64(void)
{
    char buf[10] = {0};
    __int128 x = wide_input();
    if (x >= 0 && x < -TWO_TO_THE_64 + 20)
        return buf[x];
    return 0;
}

// Never runs: no long is below LONG_MIN. LONG_MIN - 1 wrapped to LONG_MAX and the access was
// reported as going up to 9223372036854775807.
char below_long_min(long v)
{
    char buf[10] = {0};
    if (v < (-9223372036854775807L - 1))
        return buf[v];
    return 0;
}

// Every predicate against a constant wider than 64 bits, signed and unsigned.
int wide_predicates(void)
{
    __int128 s = wide_input();
    unsigned __int128 u = (unsigned __int128)wide_input();
    if (s < TWO_TO_THE_64)
        return 1;
    if (s <= TWO_TO_THE_64)
        return 2;
    if (s > TWO_TO_THE_64)
        return 3;
    if (s >= TWO_TO_THE_64)
        return 4;
    if (s == TWO_TO_THE_64)
        return 5;
    if (u < UNSIGNED_TWO_TO_THE_64)
        return 6;
    if (u <= UNSIGNED_TWO_TO_THE_64)
        return 7;
    if (u > UNSIGNED_TWO_TO_THE_64)
        return 8;
    if (u >= UNSIGNED_TWO_TO_THE_64)
        return 9;
    return 0;
}

// A constant that fits still gives its bound.
char up_to_ten(long x)
{
    char buf[10] = {0};
    // at line 68, column 16
    // [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 10)
    // ↳ alias path: buf
    // ↳ index variable may go up to 10 (array last valid index: 9)
    // ↳ (this is a read access)
    if (x <= 10)
        return buf[x];
    return 0;
}

// not contains: index variable may go up to 19
// not contains: index variable may go up to 9223372036854775807
