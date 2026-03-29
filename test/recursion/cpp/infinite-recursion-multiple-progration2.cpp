// SPDX-License-Identifier: Apache-2.0
#include <iostream>

void foo();
void foobar();

void foobar()
{
    foo();
}

void bar()
{
    foobar();
}

void foo(void)
{
    bar();
}

int main(void)
{
    foo();

    return 0;
}

// at line 8, column 5
// [ !Info! ] recursive or mutually recursive function detected

// at line 8, column 5
// [!!!Error] unconditional self recursion detected (no base case)
//          ↳ this will eventually overflow the stack at runtime

// at line 18, column 5
// [ !Info! ] recursive or mutually recursive function detected

// at line 18, column 5
// [!!!Error] unconditional self recursion detected (no base case)
//          ↳ this will eventually overflow the stack at runtime

// at line 13, column 5
// [ !Info! ] recursive or mutually recursive function detected

// at line 13, column 5
// [!!!Error] unconditional self recursion detected (no base case)
//          ↳ this will eventually overflow the stack at runtime