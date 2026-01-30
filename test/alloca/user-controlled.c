#include <alloca.h>
#include <stddef.h>

void foo(size_t n)
{
    // at line 11, column 24
    // [!!] user-controlled alloca size for variable 'buf'
    //     allocation performed via alloca/VLA; stack usage grows with runtime value
    //     size is unbounded at compile time
    //     size depends on user-controlled input (function argument or non-local value)
    char* buf = (char*)alloca(n);
    if (buf)
        buf[0] = 0;
}
