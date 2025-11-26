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
