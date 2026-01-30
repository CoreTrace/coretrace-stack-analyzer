#include <alloca.h>
#include <stddef.h>

void big_alloca(void)
{
    size_t n = 2 * 1024 * 1024;
    // at line 12, column 24
    // [!!] large alloca on the stack for variable 'buf'
    //     allocation performed via alloca/VLA; stack usage grows with runtime value
    //     requested stack size: 2097152 bytes
    //     exceeds safety threshold of 1048576 bytes (stack limit: 8388608 bytes)
    char* buf = (char*)alloca(n);
    if (buf)
        buf[0] = 0;
}
