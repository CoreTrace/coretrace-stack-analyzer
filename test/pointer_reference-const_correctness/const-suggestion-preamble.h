// SPDX-License-Identifier: Apache-2.0
#ifndef CONST_SUGGESTION_PREAMBLE_H
#define CONST_SUGGESTION_PREAMBLE_H

// A checked warning, reported by every compilation of the file, before and after the rewrite: it
// must not count as new.
static inline int* without_const(const int* p)
{
    return p;
}

int first_value(int* p);
int second_value(int* q);

#endif
