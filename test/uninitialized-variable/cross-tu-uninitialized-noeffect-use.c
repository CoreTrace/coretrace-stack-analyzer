// SPDX-License-Identifier: Apache-2.0
//
// Caller of peek() (#157). Alone, peek() is an unknown void function, presumed to write through
// p: nothing is reported. With cross-tu-uninitialized-noeffect-def.c, peek() writes nothing and
// the read of v is reported, as within one file. run_test.py checks each pairing.

void peek(int* p);

int uses_peek(void)
{
    int v;
    peek(&v);
    return v;
}
