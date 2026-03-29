// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>

void print_input(char* input)
{
    printf(input);
}

int main()
{
    char user_input[100];
    printf("Entrez une chaîne : ");
    fgets(user_input, 100, stdin);
    print_input(user_input);
    return 0;
}

// at line 5, column 12
// [ !!Warn ] non-literal format string may allow format injection

// at line 10, column 1
// [ !!Warn ] local variable 'user_input' is never initialized
