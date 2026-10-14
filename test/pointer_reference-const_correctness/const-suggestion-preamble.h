// SPDX-License-Identifier: Apache-2.0
#ifndef CONST_SUGGESTION_PREAMBLE_H
#define CONST_SUGGESTION_PREAMBLE_H

// A checked warning in the preamble: a compilation that reuses the preamble does not report it,
// one that parses the file whole does.
static inline int* without_const(const int* p)
{
    return p;
}

int first_value(int* p);
int second_value(int* q);

#endif
