// SPDX-License-Identifier: Apache-2.0
// #217: __PRETTY_FUNCTION__ prints the function's signature, which the suggested type changes. The
// suggestion is kept, and the message says what the program prints differently. Any other use of
// that text, such as its size, is a change of the generated code.

// strict-expectation-details: true
// not contains: parameter 'p' in function 'signature_size'
// not contains: ↳ ABI:
#include <stdio.h>

// at line 17, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'print_and_read' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches once debug information, metadata numbering and renamed symbols are normalized, except for the differences below
// ↳ difference: the signature printed by __PRETTY_FUNCTION__ changes, from "int print_and_read(int *)" to "int print_and_read(const int *)"
int print_and_read(int* p)
{
    puts(__PRETTY_FUNCTION__);
    return *p;
}

int signature_size(int* p)
{
    return *p ? (int)sizeof(__PRETTY_FUNCTION__) : 0;
}
