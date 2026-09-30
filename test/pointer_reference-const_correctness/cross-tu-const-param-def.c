// SPDX-License-Identifier: Apache-2.0
//
// Definitions whose first parameter points to const (#157), for the callers of
// cross-tu-const-param-use.c, cross-tu-const-param-noproto-use.c and
// cross-tu-const-param-asm-use.c. read_pair() takes two parameters.

int read_only(const int* p)
{
    return *p;
}

int read_pair(const int* p, int n)
{
    return *p == n;
}
