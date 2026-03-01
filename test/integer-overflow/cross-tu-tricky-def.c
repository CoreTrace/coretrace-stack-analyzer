#include <stddef.h>
#include <stdlib.h>
#include <string.h>

int io_cross_signed_overflow(int a, int b, int gate1, int gate2)
{
    if (gate1)
    {
        if (gate2)
        {
            return a + b;
        }
    }
    return a;
}

void io_cross_truncation_alloc(int cond)
{
    unsigned long big = 0x1FFFFFFFFUL;
    unsigned int small = cond ? (unsigned int)big : 32u;
    char* buf = (char*)malloc((size_t)small);
    if (buf)
    {
        free(buf);
    }
}

void io_cross_signed_to_size_copy(int len, int gate_outer, int gate_inner)
{
    char dst[32];
    for (int i = 0; i < 2; ++i)
    {
        if (gate_outer)
        {
            if (((i & 1) == 0) && gate_inner)
            {
                memcpy(dst, "BBBB", (size_t)len);
            }
        }
    }
}

// at line 8, column 22
// [ !!Warn ] potential signed integer overflow in arithmetic operation
//          ↳ operation: add
//          ↳ result is returned without a provable non-overflow bound

// at line 17, column 25
// [ !!Warn ] potential integer truncation in size computation before 'malloc'
//          ↳ narrowing conversion may drop high bits and produce a smaller buffer size

// at line 28, column 17
// [ !!Warn ] potential signed-to-size conversion before 'memcpy'
//          ↳ a possibly negative signed value is converted to an unsigned length
//          ↳ this can become a very large size value and trigger out-of-bounds access

// at line 24, column 1
// [ !!Warn ] local variable 'dst' is never initialized
//          ↳ declared without initializer and no definite write was found in this function
