#include <string.h>

// Function: foo
//   [!!] potential stack buffer overflow in memcpy on variable '<unnamed>'
//        destination stack buffer size: 10 bytes
//        requested 20 bytes to be copied/initialized
void foo(char *src)
{
    char buf[10];
    memcpy(buf, src, 20);
}

int main(void)
{
    char src[20] = {0};
    foo(src);
    return 0;
}
