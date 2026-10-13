// SPDX-License-Identifier: Apache-2.0
// #217: the suggestion is kept when the generated code is the same. Inlining a callee with a
// restrict parameter adds metadata whose numbers move with the debug information of the new type.

// strict-expectation-details: true
// not contains: ↳ ABI:

static inline __attribute__((always_inline)) int load(const int* __restrict q)
{
    return *q;
}

// at line 19, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'read_restricted' is only read, or passed where a pointer to const is expected
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ the callee's body is not checked: it may still modify the pointed object through a cast
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches once debug information, metadata numbering and renamed symbols are normalized
int read_restricted(int* p)
{
    return load(p);
}
