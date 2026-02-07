#include <stdio.h>

/*
 * 1) Simple cases: upper bound OK / not OK
 */

// NO WARNING expected (UB = 9, size = 10)
void ub_ok(int i)
{
    char buf[10];

    if (i <= 9)
        buf[i] = 'A';
}

// WARNING UB expected (UB = 10, size = 10)
void ub_overflow(int i)
{
    char buf[10];

    // at line 27, column 16
    // [!!] potential stack buffer overflow on variable 'buf' (size 10)
    //     alias path: buf
    //     index variable may go up to 10 (array last valid index: 9)
    //     (this is a write access)
    if (i <= 10)
        buf[i] = 'B';
}

/*
 * 2) Negative lower bound: index potentially < 0
 */

// WARNING negative LB expected (i >= -3 && i < 5)
void lb_negative(int i)
{
    char buf[10];

    // at line 45, column 16
    // [!!] potential negative index on variable 'buf' (size 10)
    //     alias path: buf
    //     inferred lower bound for index expression: -3 (index may be < 0)
    //     (this is a write access)
    if (i >= -3 && i < 5)
        buf[i] = 'C';
}

// WARNING negative LB + UB out of bounds (i >= -3 && i <= 15)
void lb_and_ub(int i)
{
    char buf[10];

    // at line 65, column 16
    // [!!] potential stack buffer overflow on variable 'buf' (size 10)
    //     alias path: buf
    //     index variable may go up to 15 (array last valid index: 9)
    //     (this is a write access)

    // at line 65, column 16
    // [!!] potential negative index on variable 'buf' (size 10)
    //     alias path: buf
    //     inferred lower bound for index expression: -3 (index may be < 0)
    //     (this is a write access)
    if (i >= -3 && i <= 15)
        buf[i] = 'D';
}

/*
 * 3) Nested ifs: refine the range (LB & UB)
 *
 *   if (i <= 10) {
 *       if (i > 5)
 *           buf[i] = 'E';
 *   }
 *
 * Here we know that 6 <= i <= 10
 * with buf[8] -> UB out of bounds
 */

// EXPECTED: UB out of bounds (size 8, i in [6,10])
void nested_if_overflow(int i)
{
    char buf[8];

    // at line 94, column 20
    // [!!] potential stack buffer overflow on variable 'buf' (size 8)
    //     alias path: buf
    //     index variable may go up to 10 (array last valid index: 7)
    //     (this is a write access)
    if (i <= 10)
    {
        if (i > 5)
        {
            buf[i] = 'E';
        }
    }
}

// “Safe” variant for comparison (size 16, i in [6,10]) -> ideally no warnings
void nested_if_ok(int i)
{
    char buf[16];

    if (i <= 10)
    {
        if (i > 5)
        {
            buf[i] = 'F';
        }
    }
}

/*
 * 4) Loops: classic for patterns
 */

// NO WARNING expected (0 <= i < 10, size 10)
void loop_ok(void)
{
    char buf[10];

    for (int i = 0; i < 10; ++i)
        buf[i] = 'G';
}

// WARNING UB expected (0 <= i <= 10, size = 10)
void loop_ub_overflow(void)
{
    char buf[10];

    // at line 137, column 16
    // [!!] potential stack buffer overflow on variable 'buf' (size 10)
    //     alias path: buf
    //     index variable may go up to 10 (array last valid index: 9)
    //     (this is a write access)
    for (int i = 0; i <= 10; ++i)
        buf[i] = 'H';
}

// WARNING negative LB expected (-3 <= i < 5, size = 10)
void loop_lb_negative(void)
{
    char buf[10];

    for (int i = -3; i < 5; ++i)
        buf[i] = 'I';
}

/*
 * 5) Unreachable case with out-of-bounds access
 *    (you already have this logic, but this checks we keep the info)
 */

// EXPECTED: overflow warning + [info] unreachable
void unreachable_example(void)
{
    int i = 1;
    char buf[10];

    // at line 168, column 17
    // [!!] potential stack buffer overflow on variable 'buf' (size 10)
    //     alias path: buf
    //     constant index 11 is out of bounds (0..9)
    //     (this is a write access)
    //     [info] this access appears unreachable at runtime (condition is always false for this branch)
    if (i > 10)
    { // condition false at runtime
        buf[11] = 'J';
    }
}

/*
 * 6) Pointer aliasing + range (LB & UB)
 */

// EXPECTED: UB + negative LB (p = buf)
void alias_lb_ub(int i)
{
    char buf[10];
    char* p = buf;

    // at line 194, column 14
    // [!!] potential stack buffer overflow on variable 'buf' (size 10)
    //     alias path: buf -> arraydecay -> p
    //     index variable may go up to 12 (array last valid index: 9)
    //     (this is a write access)

    // at line 194, column 14
    // [!!] potential negative index on variable 'buf' (size 10)
    //     alias path: p -> arraydecay -> buf
    //     inferred lower bound for index expression: -2 (index may be < 0)
    //     (this is a write access)
    if (i >= -2 && i <= 12)
        p[i] = 'K';
}

// EXPECTED: no warning (0 <= i < 10)
void alias_ok(int i)
{
    char buf[10];
    char* p = buf;

    if (i >= 0 && i < 10)
        p[i] = 'L';
}

/*
 * 7) Weird combination: tight bounds, but still safe
 *    i in [2,7], buf[8] -> normally OK
 */

void tight_range_ok(int i)
{
    char buf[8];

    if (i >= 2 && i <= 7)
        buf[i] = 'M';
}

/*
 * 8) Extreme case: very wide bounds
 *    i >= -100 && i <= 100, buf[10] -> negative LB + UB out of bounds
 */

void huge_range(int i)
{
    char buf[10];

    // at line 241, column 16
    // [!!] potential stack buffer overflow on variable 'buf' (size 10)
    //     alias path: buf
    //     index variable may go up to 100 (array last valid index: 9)
    //     (this is a write access)

    // at line 241, column 16
    // [!!] potential negative index on variable 'buf' (size 10)
    //     alias path: buf
    //     inferred lower bound for index expression: -100 (index may be < 0)
    //     (this is a write access)
    if (i >= -100 && i <= 100)
        buf[i] = 'N';
}

/*
 * main: just to prevent the compiler from optimizing everything away
 */

int main(void)
{
    ub_ok(5);
    ub_overflow(10);

    lb_negative(-1);
    lb_and_ub(20);

    nested_if_overflow(8);
    nested_if_ok(8);

    loop_ok();
    loop_ub_overflow();
    loop_lb_negative();

    unreachable_example();

    alias_lb_ub(0);
    alias_ok(5);

    tight_range_ok(3);
    huge_range(0);

    return 0;
}
