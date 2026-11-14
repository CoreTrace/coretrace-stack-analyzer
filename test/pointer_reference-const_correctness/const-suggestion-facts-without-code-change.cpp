// SPDX-License-Identifier: Apache-2.0
// #217: a call or a constant condition that the suggested type changes refutes the suggestion,
// even when the generated code stays the same:
// - sized: sizeof names sized(bool) before the rewrite and sized(const int*) after it; both return
//   int, so the size, and the code, stay the same;
// - typed: if constexpr tests the parameter's own type, and both branches are the same code.

// not contains: parameter 'p' in function 'sized(int*)'
// not contains: parameter 'p' in function 'typed(int*)'
#include <type_traits>

int sized(int* p)
{
    return *p;
}

int sized(bool flag);

int size_of_call(const int* q)
{
    return static_cast<int>(sizeof(sized(q)));
}

int typed(int* p)
{
    if constexpr (std::is_const_v<std::remove_pointer_t<decltype(p)>>)
        return *p;
    else
        return *p;
}

// The Clang check of const suggestions is experimental: it runs only with this option.
// analyzer-arg: --experimental-const-suggestion-check
