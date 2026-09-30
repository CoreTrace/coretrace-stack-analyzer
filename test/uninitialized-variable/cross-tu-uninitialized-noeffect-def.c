// SPDX-License-Identifier: Apache-2.0
//
// peek() for cross-tu-uninitialized-noeffect-use.c (#157): it writes nothing through p.

void peek(int* p)
{
    (void)p;
}

// at line 5, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'peek' is never used to modify the pointed object
//             ↳ current type: int *p
//             ↳ suggested type: const int *p
