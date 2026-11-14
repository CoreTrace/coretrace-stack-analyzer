// SPDX-License-Identifier: Apache-2.0
// #217 (D6): the project turns its warnings into errors (-Werror) and disables a warning group
// that the check tracks, which this file triggers: strip returns a pointer to const as a plain
// pointer. The check keeps each tracked group a warning in both of its compilations: the file
// compiles as it is, and the suggestion for read_value is checked.

// strict-expectation-details: true

// at line 20, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'read_value' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside

// The project's compile command, which both the analysis and the check use.
// analyzer-arg: --compile-arg=-Werror
// analyzer-arg: --compile-arg=-Wno-incompatible-pointer-types-discards-qualifiers
// analyzer-arg: --experimental-const-suggestion-check

int read_value(int* p)
{
    return *p;
}

char* strip(const char* s)
{
    return s;
}
