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
