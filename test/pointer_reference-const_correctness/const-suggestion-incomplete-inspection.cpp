// SPDX-License-Identifier: Apache-2.0
// #217: the suggested type can make a compilation stop at a fatal error, after which the code it
// would have parsed is not inspected. Here sizeof(sized(p)) selects sized(int*) before the rewrite,
// and the template once p points to const: deducing its return type instantiates Deep without end.
// What the rewrite changes is then unknown: the suggestion is kept, not checked.

// strict-expectation-details: true

template <typename T> struct Deep
{
    static constexpr int value = Deep<T*>::value + 1;
};

int sized(int* p);

template <typename T> auto sized(const T* p)
{
    return Deep<T>::value + *p;
}

// at line 26, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'read_sized(int*)' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ not checked with Clang: the rewritten code cannot be inspected entirely, a fatal error stops its compilation:
int read_sized(int* p)
{
    return *p + static_cast<int>(sizeof(sized(p)));
}

// The Clang check of const suggestions is experimental: it runs only with this option.
// analyzer-arg: --experimental-const-suggestion-check
