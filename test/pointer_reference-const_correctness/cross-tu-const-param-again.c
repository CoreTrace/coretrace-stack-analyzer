// SPDX-License-Identifier: Apache-2.0
//
// A second read_only() whose parameter points to const (#157), as when two programs are analyzed
// together. Every definition of the symbol declares it so: the callers of
// cross-tu-const-param-use.c keep the fact.

int read_only(const int* p)
{
    return p ? *p : 0;
}
