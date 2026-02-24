#include <unistd.h>
#include <stdlib.h>

int main(void)
{
    char tmp[1024];

    ssize_t n = read(STDIN_FILENO, tmp, sizeof(tmp));
    if (n <= 0)
        return 1;

    // char *buf = malloc(n);
    int len = (int)n;

    char buf[len];
    if (!buf)
        return 1;

    for (ssize_t i = 0; i < n; ++i)
        buf[i] = tmp[i];

    free(buf);
    return 0;
}

// at line 15, column 5
// [ !!Warn ] dynamic stack allocation detected for variable 'vla'
// ↳ allocated type: i8
// ↳ size of this allocation is not compile-time constant (VLA / variable alloca) and may lead to unbounded stack usage

// at line 15, column 5
// [ !!Warn ] user-controlled alloca size for variable 'vla'
// ↳ allocation performed via alloca/VLA; stack usage grows with runtime value
// ↳ size is unbounded at compile time
// ↳ size depends on user-controlled input (function argument or non-local value)

// not contains: potential read of uninitialized local variable 'tmp'
