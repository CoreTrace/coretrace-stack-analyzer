// SPDX-License-Identifier: Apache-2.0
//
// Callers of is_ready() and is_ready_or_abort() (#157). Alone, both are declarations without their
// definition, and nothing is reported. With cross-tu-deterministic-def.c, both are deterministic:
// each else-if repeats its if, as within one file. run_test.py checks each pairing.

int is_ready(int x);
int is_ready_or_abort(int x);

int pick(int x)
{
    if (is_ready(x))
        return 1;
    else if (is_ready(x))
        return 2;
    return 0;
}

int pick_or_abort(int x)
{
    if (is_ready_or_abort(x))
        return 1;
    else if (is_ready_or_abort(x))
        return 2;
    return 0;
}
