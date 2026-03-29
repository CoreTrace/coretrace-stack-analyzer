// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>

// at line 11, column 5
// [ !Info! ] recursive or mutually recursive function detected

// at line 11, column 5
// [!!!Error] unconditional self recursion detected (no base case)
// ↳ this will eventually overflow the stack at runtime
void tutu(void)
{
    tutu();
}

int main(void)
{
    tutu();

    return 0;
}
