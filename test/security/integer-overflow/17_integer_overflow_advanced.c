/**
 * 17 - ADVANCED INTEGER OVERFLOW CASES (nested/if/loop/tricky)
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* 17a. Signed overflow in nested if branch returned directly */
int vuln_signed_overflow_nested_if(int a, int b, int gate1, int gate2)
{
    if (gate1)
    {
        if (gate2)
        {
            return a + b;
        }
    }
    return 0;
}

/* 17b. Truncation hidden in select expression before allocation */
void vuln_truncation_select_tricky(int cond)
{
    unsigned long big = 0x1FFFFFFFFUL;
    unsigned int small = cond ? (unsigned int)big : 64u;
    char* buf = (char*)malloc((size_t)small);
    if (buf)
    {
        free(buf);
    }
}

/* 17c. Signed-to-size conversion in nested loop and nested if */
void vuln_signed_to_size_nested_loop(int len, int n)
{
    char dst[64];
    for (int i = 0; i < n; ++i)
    {
        if ((i & 1) == 0)
        {
            if (len <= 64)
            {
                memcpy(dst, "AAAA", (size_t)len);
            }
        }
    }
}

int main(void)
{
    (void)vuln_signed_overflow_nested_if(2147483647, 1, 1, 1);
    vuln_truncation_select_tricky(1);
    vuln_signed_to_size_nested_loop(-1, 2);
    return 0;
}

// run_test expectations
// resource-model: models/resource-lifetime/generic.txt
// escape-model: models/stack-escape/generic.txt
// buffer-model: models/buffer-overflow/generic.txt

// at line 13, column 22
// [ !!Warn ] potential signed integer overflow in arithmetic operation
//             ↳ operation: add
//             ↳ result is returned without a provable non-overflow bound

// at line 23, column 25
// [ !!Warn ] potential integer truncation in size computation before 'malloc'
//             ↳ narrowing conversion may drop high bits and produce a smaller buffer size

// at line 35, column 17
// [ !!Warn ] potential signed-to-size conversion before 'memcpy'
//             ↳ a possibly negative signed value is converted to an unsigned length
//             ↳ this can become a very large size value and trigger out-of-bounds access

// at line 31, column 1
// [ !!Warn ] local variable 'dst' is never initialized
