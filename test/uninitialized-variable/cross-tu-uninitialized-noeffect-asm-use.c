// SPDX-License-Identifier: Apache-2.0
//
// Calls peek() under another C name, through an assembler name (#157). For the linker it is the
// same symbol, on Linux as on macOS: the summary of cross-tu-uninitialized-noeffect-def.c applies.

#define STR2(x) #x
#define STR(x) STR2(x)

void peek_under_another_name(int* p) __asm__(STR(__USER_LABEL_PREFIX__) "peek");

int uses_asm_name(void)
{
    int v;
    peek_under_another_name(&v);
    return v;
}
