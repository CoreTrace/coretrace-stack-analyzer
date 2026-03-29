// SPDX-License-Identifier: Apache-2.0

#include <stddef.h>

extern void custom_copy(void* dst, const void* src, size_t n);

void foo(const char* src)
{
    char buf[8] = {0};
    custom_copy(buf, src, 32);
}

// not contains: potential stack buffer overflow in
