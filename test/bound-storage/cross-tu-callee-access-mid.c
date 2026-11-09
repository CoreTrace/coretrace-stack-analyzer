// SPDX-License-Identifier: Apache-2.0
//
// A relay between cross-tu-callee-access-use.c and cross-tu-callee-access-leaf.c (#266): it
// copies its parameter and passes it on. Alone, write_first() is a declaration, and nothing is
// reported.

void write_first(char* p);

void relay(char* p)
{
    char* q = p;
    write_first(q);
}
