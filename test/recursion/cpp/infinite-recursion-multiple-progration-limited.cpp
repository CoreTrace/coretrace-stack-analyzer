// SPDX-License-Identifier: Apache-2.0
#include <iostream>

void tutu();

// at line 11, column 9
// [ !Info! ] recursive or mutually recursive function detected
void titi()
{
    static int test = 0;

    test++;
    if (test == 3)
        return;
    tutu();
}

// at line 21, column 5
// [ !Info! ] recursive or mutually recursive function detected
void tutu(void)
{
    titi();
}

int main(void)
{
    tutu();

    return 0;
}
