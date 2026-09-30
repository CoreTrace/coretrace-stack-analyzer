// SPDX-License-Identifier: Apache-2.0
//
// A weak peek() that writes nothing (#157). The linker may retain another definition of the
// symbol instead, so it proves nothing about the one that runs.

__attribute__((weak)) void peek(int* p)
{
    (void)p;
}

// at line 6, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'peek' is never used to modify the pointed object
//             ↳ current type: int *p
//             ↳ suggested type: const int *p
