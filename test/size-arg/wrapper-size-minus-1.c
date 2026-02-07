#include <string.h>
#include <stddef.h>

void test2(char* dst, const char* src, size_t n)
{
    strncpy(dst, src, n);
}

void test(char* dst, const char* src, size_t n)
{
    test2(dst, src, n);
}

void caller(char* dst, const char* src, size_t n)
{
    // at line 20, column 5
    // [!] potential unsafe write with length (size - 1) in test
    //     destination pointer may be null
    //     size operand may be <= 1
    test(dst, src, n - 1);
}
