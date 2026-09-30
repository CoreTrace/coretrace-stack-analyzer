// SPDX-License-Identifier: Apache-2.0
//
// A read_only() whose parameter does not point to const (#157). It gives the callers of
// cross-tu-const-param-use.c no fact, alone or beside the definition of
// cross-tu-const-param-def.c: a symbol is const only if every definition declares it so.

int read_only(int* p)
{
    return *p;
}

// at line 7, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'read_only' is never used to modify the pointed object
//             ↳ current type: int *p
//             ↳ suggested type: const int *p
