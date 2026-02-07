#include <string.h>
#include <stddef.h>

void foo(char* dst, const char* src, size_t n)
{
    // at line 10, column 5
    // [!] potential unsafe write with length (size - 1) in __strncpy_chk
    //     destination pointer may be null
    //     size operand may be <= 1
    strncpy(dst, src, n - 1);
}

int main(void)
{
    char a[8] = {0};
    char b[8] = {0};
    foo(a, b, 8);
    return 0;
}
