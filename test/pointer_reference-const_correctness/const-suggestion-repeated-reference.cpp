// SPDX-License-Identifier: Apache-2.0
// #217: a macro that expands its argument twice writes two references at one site, both where
// Clang expands them and where they are written. Each one is compared with its counterpart:
// sizeof names counted(bool) before the rewrite and counted(const int*) after it, twice, and the
// size, so the generated code, stays the same. The suggestion is refuted.

// not contains: parameter 'p' in function 'counted(int*)'

int counted(int* p)
{
    return *p;
}

int counted(bool flag);

#define BOTH(e) (sizeof(e) + sizeof(e))

int size_of_both(const int* q)
{
    return static_cast<int>(BOTH(counted(q)));
}

// The Clang check of const suggestions is experimental: it runs only with this option.
// analyzer-arg: --experimental-const-suggestion-check
