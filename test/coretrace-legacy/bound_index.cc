// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>

int a[10];

int main(int argc, char* argv[])
{
    size_t i = 0;
    for (; i < 10; i++)
    {
        a[i] = i;
    }
    a[i] = i;
    printf("%i", a[i]);
}

// at line 12, column 10
// [ !!Warn ] potential buffer overflow on global variable 'a' (size 10)

// at line 13, column 18
// [ !!Warn ] potential buffer overflow on global variable 'a' (size 10)
