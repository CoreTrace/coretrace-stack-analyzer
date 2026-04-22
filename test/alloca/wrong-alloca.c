#include <alloca.h>
#include <stdint.h>
#include <stddef.h>

int foo(uint8_t small_size)
{
    size_t size_allocation = (size_t)small_size * 1024;

    char* buff = (char*)alloca(size_allocation);

    if (!buff)
        goto error;

    return 0;

error:

    return 1;
}

// at line 9, column 25
// [ !!Warn ] dynamic stack allocation detected for variable 'buff'
//             ↳ allocated type: i8
//             ↳ size of this allocation is not compile-time constant (VLA / variable alloca) and may lead to unbounded stack usage

// at line 9, column 25
// [ !!Warn ] user-controlled alloca size for variable 'buff'
//             ↳ allocation performed via alloca/VLA; stack usage grows with runtime value
//             ↳ size is unbounded at compile time
//             ↳ size depends on user-controlled input (function argument or non-local value)
