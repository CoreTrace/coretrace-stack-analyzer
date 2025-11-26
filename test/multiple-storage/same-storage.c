#include <string.h>

void foo(void)
{
    char buf[10];
    buf[0] = 'a';
    buf[1] = 'b';
    buf[2] = 'c';
    buf[2] = 'c';
}

int main(void)
{
    char src[20] = {0};
    foo();
    return 0;
}
