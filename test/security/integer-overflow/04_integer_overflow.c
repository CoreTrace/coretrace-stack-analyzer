/**
 * 04 - INTEGER OVERFLOW / UNDERFLOW (CWE-190, CWE-191, CWE-195, CWE-197)
 *
 * Compile: gcc -Wall -Wextra -g -fsanitize=undefined 04_integer_overflow.c -o 04_test
 * Analyze: clang --analyze 04_integer_overflow.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* 4a. Integer overflow menant à un petit malloc */
void vuln_integer_overflow_alloc(unsigned int count, unsigned int elem_size)
{
    unsigned int total = count * elem_size; /* CWE-190: overflow silencieux */
    char* buf = (char*)malloc(total);
    if (!buf)
        return;
    memset(buf, 0, count * elem_size); /* écrit plus que total si overflow */
    free(buf);
}

/* 4b. Signed integer overflow (UB) */
int vuln_signed_overflow(int a, int b)
{
    return a + b; /* CWE-190: undefined behavior si overflow */
}

/* 4c. Troncature implicite lors d'un cast */
void vuln_truncation(void)
{
    unsigned long big = 0x1FFFFFFFF;
    unsigned int small = (unsigned int)big; /* CWE-197: perte de bits hauts */
    char* buf = (char*)malloc(small);       /* allocation trop petite */
    if (buf)
    {
        memset(buf, 'A', big); /* heap overflow massif */
        free(buf);
    }
}

/* 4d. Signedness mismatch : int négatif → size_t énorme */
void vuln_signedness(int len)
{
    char buf[100];
    if (len > 100)
        return; /* semble sûr... */
    /* mais len négatif passe la vérif et est converti en size_t énorme */
    memcpy(buf, "AAAA", (size_t)len); /* CWE-195 */
}

int main(void)
{
    printf("=== 04: Integer Overflow Tests ===\n");
    vuln_integer_overflow_alloc(0x40000001, 4);
    printf("signed overflow: %d\n", vuln_signed_overflow(INT_MAX, 1));
    /* vuln_truncation();  -- dangereux à exécuter */
    vuln_signedness(-1);
    return 0;
}

// run_test expectations
// resource-model: models/resource-lifetime/generic.txt
// escape-model: models/stack-escape/generic.txt
// buffer-model: models/buffer-overflow/generic.txt

// at line 16, column 25
// [ !!Warn ] potential integer overflow in size computation before 'malloc'
//             ↳ operation: mul
//             ↳ overflowed size may under-allocate memory or make bounds checks unsound

// at line 18, column 5
// [ !!Warn ] potential integer overflow in size computation before 'memset'
//             ↳ operation: mul
//             ↳ overflowed size may under-allocate memory or make bounds checks unsound

// at line 24, column 14
// [ !!Warn ] potential signed integer overflow in arithmetic operation
//             ↳ operation: add
//             ↳ result is returned without a provable non-overflow bound

// at line 31, column 25
// [ !!Warn ] potential integer truncation in size computation before 'malloc'
//             ↳ narrowing conversion may drop high bits and produce a smaller buffer size

// at line 43, column 5
// [ !!Warn ] potential signed-to-size conversion before 'memcpy'
//             ↳ a possibly negative signed value is converted to an unsigned length
//             ↳ this can become a very large size value and trigger out-of-bounds access

// at line 40, column 1
// [ !!Warn ] local variable 'buf' is never initialized
//          ↳ declared without initializer and no definite write was found in this function
