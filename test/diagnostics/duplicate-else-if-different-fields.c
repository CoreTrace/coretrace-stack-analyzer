// SPDX-License-Identifier: Apache-2.0
// Two conditions on different fields are not duplicates, even when their GEPs have the same
// indices: s->a[1] is `gep [4 x i32], s, i64 0, i64 1` and s->b is `gep %struct.S, s, i32 0,
// i32 1`. Comparing those indices once aborted assert-enabled builds (APInt widths differ) and
// made release builds report the else-if branch as unreachable.

struct S
{
    int a[4];
    int b;
};

// s->a[1] and s->b are different fields: no duplicate.
int field_or_element(const struct S* s)
{
    if (s->a[1] == 3)
        return 1;
    else if (s->b == 3)
        return 2;
    return 0;
}

// Control: the same field twice is a duplicate, and stays reported.
int same_field_twice(const struct S* s)
{
    if (s->b == 3)
        return 1;
    else if (s->b == 3)
        return 2;
    return 0;
}

// at line 28, column 19
// [ !!Warn ] unreachable else-if branch: condition is equivalent to a previous 'if' condition
// ↳ else branch implies previous condition is false

// not contains: at line 18, column 19
