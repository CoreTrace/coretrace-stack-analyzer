// SPDX-License-Identifier: Apache-2.0
#include <string.h>

// buffer-model: models/buffer-overflow/generic.txt

void foo(char* src)
{
    char buf[8];

    // at line 13, column 5
    // [ !!Warn ] potential stack buffer overflow in strcpy on variable 'buf'
    // ↳ destination stack buffer size: 8 bytes
    // ↳ this API has no explicit size argument; destination fit cannot be proven statically
    strcpy(buf, src);
}

int main(void)
{
    char src[16] = "0123456789abcdef";
    foo(src);
    return 0;
}

// at line 7, column 1
// [ !!Warn ] local variable 'buf' is never initialized
