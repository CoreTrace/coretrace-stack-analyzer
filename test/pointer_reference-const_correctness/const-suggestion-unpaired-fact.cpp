// SPDX-License-Identifier: Apache-2.0
// #217: a fact that the rewrite leaves without a counterpart cannot be compared. Here p + Offset{}
// calls operator+(int*, Offset) before the rewrite; once p points to const, that operator no
// longer applies, and the built-in addition takes Offset through its conversion to int. The
// reference to operator+ has no counterpart after the rewrite, while the size, so the generated
// code, stays the same: the comparison is incomplete, and the suggestion is kept, not checked.
// The expectation stops before the file and the line.

// strict-expectation-details: true

struct Offset
{
    operator int() const
    {
        return 0;
    }
};

int* operator+(int*, Offset);

// at line 26, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'read(int*)' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ not checked with Clang: the facts at
int read(int* p)
{
    return *p ? static_cast<int>(sizeof(p + Offset{})) : 0;
}

// The Clang check of const suggestions is experimental: it runs only with this option.
// analyzer-arg: --experimental-const-suggestion-check
