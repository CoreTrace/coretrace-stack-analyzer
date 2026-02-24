#include <stddef.h>

extern void custom_memcpy_wrapper(void* dst, const void* src, size_t n);
extern void custom_memset_wrapper(void* dst, int value, size_t n);
extern void custom_memmove_wrapper(void* dst, const void* src, size_t n);

void test_memcpy_name(const char* src)
{
    char buf[8];
    // at line 14, column 5
    // [ !!Warn ] potential stack buffer overflow in memcpy on variable 'buf'
    //          ↳ destination stack buffer size: 8 bytes
    //          ↳ requested 16 bytes to be copied/initialized
    custom_memcpy_wrapper(buf, src, 16);
}

void test_memset_name(void)
{
    char buf[10];
    // at line 24, column 5
    // [ !!Warn ] potential stack buffer overflow in memset on variable 'buf'
    //          ↳ destination stack buffer size: 10 bytes
    //          ↳ requested 24 bytes to be copied/initialized
    custom_memset_wrapper(buf, 0, 24);
}

void test_memmove_name(const char* src)
{
    char buf[12];
    // at line 34, column 5
    // [ !!Warn ] potential stack buffer overflow in memmove on variable 'buf'
    //          ↳ destination stack buffer size: 12 bytes
    //          ↳ requested 20 bytes to be copied/initialized
    custom_memmove_wrapper(buf, src, 20);
}

int main(void)
{
    char src[32] = {0};
    test_memcpy_name(src);
    test_memset_name();
    test_memmove_name(src);
    return 0;
}
