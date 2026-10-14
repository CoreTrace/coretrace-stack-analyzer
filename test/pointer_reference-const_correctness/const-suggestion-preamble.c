// SPDX-License-Identifier: Apache-2.0
// #217: the file is compiled again for each suggestion. Its preamble, the header below, is parsed
// once and reused only while the rewrite leaves it untouched. Rewriting the declarations of
// first_value and second_value changes the header: those compilations parse the file whole, and
// are compared with the file before the rewrite parsed whole too. A preamble reused regardless
// would declare the old types; comparing a whole compilation with one that reused the preamble
// would count the header's warning as new. local_value is declared in this file: its
// compilations reuse the preamble.

// strict-expectation-details: true

// at line 33, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'first_value' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside

// at line 38, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'q' in function 'second_value' is never used to modify the pointed object
// ↳ current type: int *q
// ↳ suggested type: const int *q
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside

// at line 43, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'r' in function 'local_value' is never used to modify the pointed object
// ↳ current type: int *r
// ↳ suggested type: const int *r
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
#include "const-suggestion-preamble.h"

int local_value(int* r);

int first_value(int* p)
{
    return *p;
}

int second_value(int* q)
{
    return *q;
}

int local_value(int* r)
{
    return *r;
}
