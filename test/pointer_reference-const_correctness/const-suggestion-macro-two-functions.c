// SPDX-License-Identifier: Apache-2.0
// #217: one macro invocation can declare several functions, whose names it expands at the same
// place. Each one is also known by where its name is written: the check rewrites the function the
// suggestion names and leaves the other one, which writes through its parameter, whichever the
// invocation declares first.

// strict-expectation-details: true

#define DECLARE_TWO(a, Ta, b, Tb)                                                                  \
    int a(Ta p);                                                                                   \
    int b(Tb q);

DECLARE_TWO(read_first, int*, write_first, int*)
DECLARE_TWO(write_second, int*, read_second, int*)

// at line 21, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'read_first' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
int read_first(int* p)
{
    return *p;
}

int write_first(int* q)
{
    *q = 1;
    return 0;
}

int write_second(int* p)
{
    *p = 2;
    return 0;
}

// at line 43, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'q' in function 'read_second' is never used to modify the pointed object
// ↳ current type: int *q
// ↳ suggested type: const int *q
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
int read_second(int* q)
{
    return *q;
}

// The Clang check of const suggestions is experimental: it runs only with this option.
// analyzer-arg: --experimental-const-suggestion-check
