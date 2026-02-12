#include <stdio.h>

int main(int argc, char** argv)
{
    int value;

    for (int i = 1; i < argc; ++i)
    {
        printf("Argument: %s\n", argv[i]);
        value = i; // just to use 'value' and avoid unused variable warning
    }
    printf("%d\n", value);
    return 0;
}

// at line 12, column 20
// [ !!Warn ] potential read of uninitialized local variable 'value'
// ↳ this load may execute before any definite initialization on all control-flow paths

// not contains: potential read of uninitialized local variable 'argc.addr'
// not contains: potential read of uninitialized local variable 'argv.addr'
