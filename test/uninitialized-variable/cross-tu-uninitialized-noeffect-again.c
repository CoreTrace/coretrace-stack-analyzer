// SPDX-License-Identifier: Apache-2.0
//
// A second definition of peek() that writes nothing (#157). A symbol defined by several files
// never proves "writes nothing": a caller may see one definition before the others.

void peek(int* p)
{
    (void)p;
}

// at line 6, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'peek' is never used to modify the pointed object
//             ↳ current type: int *p
//             ↳ suggested type: const int *p
