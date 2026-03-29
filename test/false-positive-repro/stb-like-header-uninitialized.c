// SPDX-License-Identifier: Apache-2.0
typedef unsigned char u8;

typedef struct Z
{
    int num_bits;
    unsigned int code_buffer;
} Z;

extern u8 zget8(Z* a);

int fp_stb_like_header_uninitialized(Z* a)
{
    u8 header[4];
    int len;
    int k;

    if (a->num_bits & 7)
        ;

    k = 0;
    while (a->num_bits > 0)
    {
        header[k++] = (u8)(a->code_buffer & 255u);
        a->code_buffer >>= 8;
        a->num_bits -= 8;
    }

    while (k < 4)
        header[k++] = zget8(a);

    len = header[1] * 256 + header[0];
    return len;
}

// at line 31, column 11
// [ !!Warn ] potential read of uninitialized local variable 'header'

// at line 31, column 29
// [ !!Warn ] potential read of uninitialized local variable 'header'
