// SPDX-License-Identifier: Apache-2.0
typedef unsigned char u8;

typedef struct Huff
{
    unsigned short firstcode[17];
    unsigned short firstsymbol[17];
    unsigned int maxcode[17];
    u8 size[288];
} Huff;

int fp_stb_like_next_code_uninitialized(Huff* z, const u8* sizelist, int num)
{
    int i;
    int k = 0;
    int code;
    int next_code[16];
    int sizes[17];

    for (i = 0; i < 17; ++i)
        sizes[i] = 0;
    for (i = 0; i < num; ++i)
        ++sizes[sizelist[i]];

    code = 0;
    for (i = 1; i < 16; ++i)
    {
        next_code[i] = code;
        z->firstcode[i] = (unsigned short)code;
        z->firstsymbol[i] = (unsigned short)k;
        code = (code + sizes[i]);
        z->maxcode[i] = (unsigned int)(code << (16 - i));
        code <<= 1;
        k += sizes[i];
    }

    for (i = 0; i < num; ++i)
    {
        int s = sizelist[i];
        if (s)
        {
            int c = next_code[s] - z->firstcode[s] + z->firstsymbol[s];
            z->size[c] = (u8)s;
            ++next_code[s];
        }
    }

    return 1;
}

// at line 41, column 21
// [ !!Warn ] potential read of uninitialized local variable 'next_code'

// at line 43, column 13
// [ !!Warn ] potential read of uninitialized local variable 'next_code'

// at line 22, column 9
// [ !!Warn ] potential read of uninitialized local variable 'sizes'

// at line 30, column 24
// [ !!Warn ] potential read of uninitialized local variable 'sizes'

// at line 33, column 14
// [ !!Warn ] potential read of uninitialized local variable 'sizes'
