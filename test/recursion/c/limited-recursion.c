// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>

// at line 8, column 12
// [ !Info! ] recursive or mutually recursive function detected
void tutu(void)
{
    static int counter = 0;
    counter++;
    if (counter == 5)
        return;
    tutu();
}

int main(void)
{
    tutu();

    return 0;
}
