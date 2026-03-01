#include <string.h>

void foo(char* src)
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

// skip
// // at line 6, column 5
// //         [ !!Warn ] potential stack buffer overflow in memcpy on variable 'buf'
// //          ↳ destination stack buffer size: 10 bytes
// //          ↳ requested 20 bytes to be copied/initialized

// // at line 5, column 1
// // [ !!Warn ] local variable 'buf' is never initialized
// //          ↳ declared without initializer and no definite write was found in this function

// at line 6, column 5
// [ !!Warn ] potential stack buffer overflow in memcpy on variable 'buf'

// at line 5, column 1
// [ !!Warn ] local variable 'buf' is never initialized
