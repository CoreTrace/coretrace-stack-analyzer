// SPDX-License-Identifier: Apache-2.0
//
// A weak my_copy() (#157). The linker may retain another definition of the symbol instead, so it
// proves nothing about the one that runs.

#include <string.h>

__attribute__((weak)) void my_copy(char* dst, const char* src, size_t n)
{
    strncpy(dst, src, n);
}
