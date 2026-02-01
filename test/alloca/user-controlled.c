#include <alloca.h>
#include <stddef.h>

void foo(size_t n)
{
    // at line 16, column 24
    // [!] dynamic stack allocation detected for variable 'buf'
    //     allocated type: i8
    //     size of this allocation is not compile-time constant (VLA / variable alloca) and may lead to unbounded stack usage

    // at line 16, column 24
    // [!!] user-controlled alloca size for variable 'buf'
    //     allocation performed via alloca/VLA; stack usage grows with runtime value
    //     size is unbounded at compile time
    //     size depends on user-controlled input (function argument or non-local value)
    char* buf = (char*)alloca(n);
    if (buf)
        buf[0] = 0;
}
