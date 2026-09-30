// SPDX-License-Identifier: Apache-2.0
//
// Calls read_only() under another C name, through an assembler name (#157). For the linker it is
// the same symbol, on Linux as on macOS: the fact of cross-tu-const-param-def.c applies.

#define STR2(x) #x
#define STR(x) STR2(x)

int read_under_another_name(const int* p) __asm__(STR(__USER_LABEL_PREFIX__) "read_only");

int uses_asm_name(int* p)
{
    return read_under_another_name(p);
}
