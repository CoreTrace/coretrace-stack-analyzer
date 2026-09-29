// SPDX-License-Identifier: Apache-2.0
//
// Max stack of functions in a call cycle (#159). Their depth is not bounded, so their max stack
// is unknown, with a lower bound that does not depend on the order of their definitions.
// Driven by check_cycle_max_stack() in run_test.py.

// A cycle defined c, then d.
int cycle_d(const char* message);

int cycle_c(const char* message)
{
    if (!message)
        return 0;
    return cycle_d(message);
}

int cycle_d(const char* message)
{
    return cycle_c(message);
}

// The same cycle, defined f, then e: it must get the same values.
int other_e(const char* message);

int other_f(const char* message)
{
    return other_e(message);
}

int other_e(const char* message)
{
    if (!message)
        return 0;
    return other_f(message);
}

// Calls into the cycle: unknown as well.
int enter_cycle(const char* message)
{
    return cycle_c(message);
}

// Calls into the cycle through enter_cycle: unknown too.
int enter_twice(const char* message)
{
    return enter_cycle(message);
}

// Calls itself.
int self_loop(int n)
{
    return n ? self_loop(n - 1) : 0;
}

// Outside any cycle: known, as before.
int leaf(void)
{
    volatile char pad[16] = {0};
    return pad[0];
}

int caller(void)
{
    return leaf();
}
