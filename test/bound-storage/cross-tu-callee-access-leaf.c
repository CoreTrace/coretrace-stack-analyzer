// SPDX-License-Identifier: Apache-2.0
//
// Callees of cross-tu-callee-access-use.c (#266). Alone, nothing calls them with an array, and
// nothing is reported. run_test.py checks each pairing.

void write_first(char* p)
{
    *p = 42;
}

char read_first(const char* p)
{
    return *p;
}
