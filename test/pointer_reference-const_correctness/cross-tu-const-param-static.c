// SPDX-License-Identifier: Apache-2.0
//
// A static read_only() whose parameter points to const (#157). It belongs to this file only: it
// gives no fact to the read_only() that cross-tu-const-param-use.c calls, and it does not count as
// one of its definitions.

static int read_only(const int* p)
{
    return *p;
}

int local_reader(void)
{
    int value = 1;
    return read_only(&value);
}
