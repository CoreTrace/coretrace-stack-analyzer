// SPDX-License-Identifier: Apache-2.0
// #217: in C++, const on the pointee changes the symbol of a function with external linkage.
// Code compiled separately against the current declaration no longer links: the message says so.

// strict-expectation-details: true

// at line 13, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'external_read(int*)' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
// ↳ ABI: the suggested type changes the symbol (_Z13external_readPi -> _Z13external_readPKi): code compiled separately must use the updated declaration and be recompiled
int external_read(int* p)
{
    return *p;
}
