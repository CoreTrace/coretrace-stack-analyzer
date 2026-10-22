// SPDX-License-Identifier: Apache-2.0
// #217: one macro invocation declares the function and a variable whose type its argument also
// writes. The rewrite changes the variable's type, which then contradicts the variable's other
// declaration: the variable becomes invalid, and the file no longer compiles. That error comes
// from a declaration outside the plan: it does not refute the suggestion, which is kept, not
// checked. The expectation stops before the file and the line.

// strict-expectation-details: true

extern int* read_value_slot;

#define PAIR(name, type)                                                                           \
    int name(type p);                                                                              \
    type name##_slot;

PAIR(read_value, int*)

// at line 23, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'read_value' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ not checked with Clang: the rewrite also changes the type of read_value_slot (
int read_value(int* p)
{
    return *p;
}

// The Clang check of const suggestions is experimental: it runs only with this option.
// analyzer-arg: --experimental-const-suggestion-check
