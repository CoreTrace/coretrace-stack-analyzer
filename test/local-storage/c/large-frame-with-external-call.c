// SPDX-License-Identifier: Apache-2.0
// A frame that already exceeds the limit on its own must still be reported
// when the function also calls an external declaration: the max stack is a
// lower bound, and a lower bound above the limit is a certain overflow.
#include <stdio.h>

int main(void)
{
    // stack-limit: 30
    // at line 12, column 9
    // [!!!Error] potential stack overflow: exceeds limit of 30 bytes
    //          ↳ locals: 5 variables (total 32 bytes)
    int a = 1;
    int b = 2;
    int c = 3;
    int d = 4;
    printf("%d\n", a + b + c + d);
    return 0;
}

// not contains: max stack (including callees): 32 bytes
