#include <stdio.h>

struct S
{
    char buf[10];
    int x;
};

void ok_direct(void)
{
    struct S s;
    for (int i = 0; i < 10; ++i)
        s.buf[i] = 'A'; // OK
}

void overflow_eq_10(void)
{
    struct S s;
    // at line 24, column 18
    // [!!] potential stack buffer overflow on variable 's' (size 10)
    //     alias path: s -> buf
    //     index variable may go up to 10 (array last valid index: 9)
    //     (this is a write access)
    for (int i = 0; i <= 10; ++i)
        s.buf[i] = 'B'; // i == 10 -> overflow
}

void overflow_const_index(void)
{
    struct S s;
    // at line 35, column 15
    // [!!] potential stack buffer overflow on variable 's' (size 10)
    //     alias path: s -> buf
    //     constant index 11 is out of bounds (0..9)
    //     (this is a write access)
    s.buf[11] = 'C'; // overflow constant
}

void nested_if_overflow(void)
{
    struct S s;
    int i = 15;

    // at line 49, column 18
    // [!!] potential stack buffer overflow on variable 's' (size 10)
    //     alias path: s -> buf
    //     index variable may go up to 15 (array last valid index: 9)
    //     (this is a write access)
    if (i > 5 && i <= 15) // UB = 15
        s.buf[i] = 'D';   // overflow
}

int main(void)
{
    ok_direct();
    overflow_eq_10();
    overflow_const_index();
    nested_if_overflow();
    return 0;
}
