#include <stdio.h>

int main(void)
{
    int n;
    // --- at line 9, column 9
    // [!!] stack pointer escape: address of variable 'n' escapes this function
    //     address passed as argument to function 'scanf' (callee may capture the pointer beyond this function)
    if (scanf("%d", &n) != 1)
        return 1;

    // --- at line 16, column 5
    // [!] dynamic stack allocation detected for variable 'vla'
    //     allocated type: i8
    //     size of this allocation is not compile-time constant (VLA / variable alloca) and may lead to unbounded stack usage
    char buf[n]; // VLA too

    return 0;
}
