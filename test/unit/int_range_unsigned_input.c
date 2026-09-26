// SPDX-License-Identifier: Apache-2.0
// Input of testProgramPointRangesUnsignedReadings. Each function has one array access; the
// test asks what is known about the index there, read as signed or as unsigned.

// (unsigned)i > 10u holds for i = -5: only i > -10 and i < 16 bound the signed reading.
void ugt_guard(int i)
{
    char buf[16];
    if (i > -10 && (unsigned)i > 10u && i < 16)
        buf[i] = 1;
}

// (unsigned)i < 16u holds only for i in [0, 15].
void ult_guard(int i)
{
    char buf[16];
    if (i > -5 && (unsigned)i < 16u)
        buf[i] = 1;
}

// (unsigned)i <= 0x80000000u holds for every i >= 0 and for INT_MIN: no signed bound.
void wide_ule_guard(int i)
{
    char buf[16];
    if ((unsigned)i <= 0x80000000u)
        buf[i] = 1;
}

// An unsigned index: u > 20u bounds its unsigned reading only.
void unsigned_index(unsigned u)
{
    char buf[16];
    if (u > 20u)
        buf[u] = 1;
}

// A signed guard on an unsigned index: (int)u > 20 puts u in [21, INT_MAX] either way.
void signed_guard_unsigned_index(unsigned u)
{
    char buf[16];
    if ((int)u > 20)
        buf[u] = 1;
}

// A signed guard bounds i from above only: read as unsigned, a negative i is past INT_MAX.
void unsigned_read_of_signed_guard(int i)
{
    char buf[16];
    if (i <= 20)
        buf[(unsigned)i] = 1;
}
