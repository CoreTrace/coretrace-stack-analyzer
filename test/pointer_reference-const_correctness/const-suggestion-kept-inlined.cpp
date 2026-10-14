// SPDX-License-Identifier: Apache-2.0
// #217: the suggestion is kept when the generated code is the same. Inlining read_once, which is
// instantiated with a lambda declared in the function, names a block after it: the name holds the
// function's symbol, which the rewrite changes. Names of values and blocks are not code.

// strict-expectation-details: true
template <class Read> __attribute__((always_inline)) inline int read_once(Read read, bool skip)
{
    if (skip)
        return 0;
    return read();
}

// at line 20, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'read_with_lambda(int*)' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
// ↳ ABI: the suggested type changes the symbol (_Z16read_with_lambdaPi -> _Z16read_with_lambdaPKi): code compiled separately must use the updated declaration and be recompiled
int read_with_lambda(int* p)
{
    auto get = [p] { return *p; };
    return read_once(get, false);
}

// The Clang check of const suggestions is experimental: it runs only with this option.
// analyzer-arg: --experimental-const-suggestion-check
