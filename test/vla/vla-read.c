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
    // at line 18, column 5
    // [!] dynamic stack allocation detected for variable 'vla'
    //     allocated type: i8
    //     size of this allocation is not compile-time constant (VLA / variable alloca) and may lead to unbounded stack usage
    char buf[len];
    if (!buf)
        return 1;

    for (ssize_t i = 0; i < n; ++i)
        buf[i] = tmp[i];

    free(buf);
    return 0;
}
