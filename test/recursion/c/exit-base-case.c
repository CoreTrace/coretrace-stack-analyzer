// SPDX-License-Identifier: Apache-2.0
//
// A recursion whose base case leaves through a noreturn call has a way out (#160): exit(),
// abort(), or a function the analyzer finds never to return (#153). None of these is an
// unconditional recursion.

#include <stdlib.h>

__attribute__((noreturn)) void raise_error(const char* message);

// Counts down, then exits: the recursion stops at n == 0.
void countdown(int n)
{
    if (n == 0)
        exit(0);
    countdown(n - 1);
}

// The same with abort().
void countdown_abort(int n)
{
    if (n == 0)
        abort();
    countdown_abort(n - 1);
}

// Never returns, without being declared noreturn.
static int fail(const char* message)
{
    raise_error(message);
    return 0;
}

// The base case leaves through fail().
void walk(int n)
{
    if (n == 0)
        fail("done");
    walk(n - 1);
}

// not contains: unconditional self recursion detected
