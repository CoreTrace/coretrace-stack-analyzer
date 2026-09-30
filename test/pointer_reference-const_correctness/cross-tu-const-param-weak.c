// SPDX-License-Identifier: Apache-2.0
//
// A weak read_only() whose parameter points to const (#157). The linker may retain another
// definition of the symbol instead, so it promises nothing about the one that runs.

__attribute__((weak)) int read_only(const int* p)
{
    return *p;
}
