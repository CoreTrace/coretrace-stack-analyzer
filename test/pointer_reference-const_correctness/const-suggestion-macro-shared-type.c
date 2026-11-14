// SPDX-License-Identifier: Apache-2.0
// #217: a macro argument can write the type of several parameters. Rewriting the type of `src`
// where it is written would rewrite `dst` too, which copy_one writes through: the rewrite would
// reach beyond the suggestion. The suggestion is kept, not checked.

// strict-expectation-details: true

#define DECLARE_COPY(name, T) int name(T src, T dst)

// at line 15, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'src' in function 'copy_one' is never used to modify the pointed object
// ↳ current type: int *src
// ↳ suggested type: const int *src
// ↳ not checked with Clang: the parameter's type is written in a macro argument that writes other parameters too
DECLARE_COPY(copy_one, int*)
{
    *dst = *src;
    return 0;
}

// The Clang check of const suggestions is experimental: it runs only with this option.
// analyzer-arg: --experimental-const-suggestion-check
