#include <stddef.h>

void telemetry_memcpy(char* dst, const char* src, size_t n)
{
    (void)src;
    (void)n;
    dst[0] = 'X';
}

void run_name_false_positive_case(const char* src)
{
    char buf[8] = {0};
    telemetry_memcpy(buf, src, 999);
}

// not contains: potential stack buffer overflow in telemetry_memcpy
