// SPDX-License-Identifier: Apache-2.0
// #217: the base-object constructor (C2) of a class with a virtual base takes a hidden `vtt` after
// `this`. The check rewrites the parameter the suggestion names, found by its index among the
// parameters the source declares: `x`, in both variants. `y`, which the constructor writes
// through, is not rewritten.

// strict-expectation-details: true

struct V
{
    int v = 0;
};

struct D : virtual V
{
    int a, b;
    D(int* x, int* y);
};

// at line 33, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'x' in function 'D::D(int*, int*)' is never used to modify the pointed object
// ↳ current type: int *x
// ↳ suggested type: const int *x
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
// ↳ ABI: the suggested type changes the symbol (_ZN1DC2EPiS0_ -> _ZN1DC2EPKiPi): code compiled separately must use the updated declaration and be recompiled

// at line 33, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'x' in function 'D::D(int*, int*)' is never used to modify the pointed object
// ↳ current type: int *x
// ↳ suggested type: const int *x
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
// ↳ ABI: the suggested type changes the symbol (_ZN1DC1EPiS0_ -> _ZN1DC1EPKiPi): code compiled separately must use the updated declaration and be recompiled
D::D(int* x, int* y) : a(*x), b(0)
{
    *y = 1;
}

int make()
{
    int p = 1, q = 2;
    D d(&p, &q);
    return d.a + q;
}

// The Clang check of const suggestions is experimental: it runs only with this option.
// analyzer-arg: --experimental-const-suggestion-check
