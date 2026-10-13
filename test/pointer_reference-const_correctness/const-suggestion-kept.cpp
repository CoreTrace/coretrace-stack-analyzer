// SPDX-License-Identifier: Apache-2.0
// #217: the suggestion is kept when the generated code is the same once the symbols the rewrite
// renames are paired. The typeinfo name of a local type holds the function's mangled name, which
// the rewrite changes with it: typeid shows it at run time, so the message says so.

// strict-expectation-details: true

// at line 15, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'read_or_throw(int*)' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches once debug information, metadata numbering and renamed symbols are normalized, except for the differences below
// ↳ difference: the name typeid gives a type named after the function changes, from "Z13read_or_throwPiE7Missing" to "Z13read_or_throwPKiE7Missing"
// ↳ ABI: the suggested type changes the symbol (_Z13read_or_throwPi -> _Z13read_or_throwPKi): code compiled separately must use the updated declaration and be recompiled
int read_or_throw(int* p)
{
    struct Missing
    {
    };
    if (!p)
        throw Missing{};
    return *p;
}
