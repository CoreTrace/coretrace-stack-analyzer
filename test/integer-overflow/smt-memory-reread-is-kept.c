// SPDX-License-Identifier: Apache-2.0
//
// Guard: two reads of a variable separated by a write are two values for the solver.
//
// The memory model gives each read one symbol per (variable, MemorySSA clobber). If the
// solver saw one variable where there are two, `before != 0` would bound `after` and hide
// these overflows.
//
// This fixture must report in BOTH passes.

int counter;
void touch(void);
void opaque_write(int* p);

// touch() may change counter: 0 before the call, INT_MAX after it, a = 1.
int reread_after_call(int a)
{
    int before = counter;
    touch();
    int after = counter;
    if (before != 0)
        return 0;
    return after + a;
}

// The same with a local whose address escapes.
int reread_local(int a)
{
    int x = 0;
    opaque_write(&x);
    int before = x;
    opaque_write(&x);
    int after = x;
    if (before != 0)
        return 0;
    return after + a;
}

// No call: two merges give x two MemoryPhis. first == 0 says nothing about second.
int two_merges(int a, int b, int c)
{
    int x;
    if (c)
        x = a;
    else
        x = b;
    int first = x;
    if (first != 0)
        return 0;
    if (c > 5)
        x = a + 1;
    else
        x = b + 1;
    int second = x;
    return second + a;
}

// at line 23, column 18
// [ !!Warn ] potential signed integer overflow in arithmetic operation
// ↳ operation: add
// ↳ result is returned without a provable non-overflow bound

// at line 36, column 18
// [ !!Warn ] potential signed integer overflow in arithmetic operation
// ↳ operation: add
// ↳ result is returned without a provable non-overflow bound

// at line 55, column 19
// [ !!Warn ] potential signed integer overflow in arithmetic operation
// ↳ operation: add
// ↳ result is returned without a provable non-overflow bound
