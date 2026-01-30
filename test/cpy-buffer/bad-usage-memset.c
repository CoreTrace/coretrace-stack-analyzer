#include <string.h>

// Function: foo
//   [!!] potential stack buffer overflow in memset on variable '<unnamed>'
//        destination stack buffer size: 10 bytes
//        requested 100 bytes to be copied/initialized
void foo(char* src)
{
    char buf[10];
    memset(buf, 0, 100);
}

int main(void)
{
    char src[20] = {0};
    foo(src);
    return 0;
}
