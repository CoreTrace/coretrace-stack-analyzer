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

//         at line 10, column 5
//                 [ !!Warn ] potential stack buffer overflow in memset on variable 'buf'
//                  ↳ destination stack buffer size: 10 bytes
//                  ↳ requested 100 bytes to be copied/initialized

//         at line 9, column 1
//         [ !!Warn ] local variable 'buf' is never initialized
//                  ↳ declared without initializer and no definite write was found in this function

//         at line 7, column 0
//   [!]ConstParameterNotModified.Pointer: parameter 'src' in function 'foo' is never used to modify the pointed object
//        current type: char *src
//        suggested type: const char *src

// at line 10, column 5
// [ !!Warn ] potential stack buffer overflow in memset on variable 'buf'
