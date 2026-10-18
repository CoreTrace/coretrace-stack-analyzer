// SPDX-License-Identifier: Apache-2.0
// #217: rewriting the declarations of first_value and second_value rewrites the header below,
// which holds a checked warning; local_value is declared in this file, and its rewrite leaves the
// header untouched. The header must be named the same way before and after a rewrite: with a
// relative input path, it was named relatively before and absolutely once the rewritten file was
// remapped, and its warning counted as new. The file name recalls the precompiled preamble, since
// removed, that this fixture also tested: the header sits in the file's preamble, its leading
// #include lines.

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

// The Clang check of const suggestions is experimental: it runs only with this option.
// analyzer-arg: --experimental-const-suggestion-check
