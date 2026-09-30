// SPDX-License-Identifier: Apache-2.0
//
// A static peek() (#157). It belongs to this file only: it is not a definition of the peek() that
// cross-tu-uninitialized-noeffect-use.c calls, and must not hide the one of
// cross-tu-uninitialized-noeffect-def.c. Within this file it writes nothing: the read is reported.

static void peek(int* p)
{
    (void)p;
}

int local_peek(void)
{
    int v;
    peek(&v);
    return v;
}

// at line 16, column 12
// [ !!Warn ] potential read of uninitialized local variable 'v'
//          ↳ this load may execute before any definite initialization on all control-flow paths

// at line 7, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'peek' is never used to modify the pointed object
//             ↳ current type: int *p
//             ↳ suggested type: const int *p
