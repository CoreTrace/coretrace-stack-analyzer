// SPDX-License-Identifier: Apache-2.0
//
// ping() and pong() call each other across two files (#157), with cross-tu-deterministic-cycle-b.c.
// A function on a cycle is never deterministic, as within one file: pick_cycle() is not reported.

int pong(int x);

int ping(int x)
{
    return x > 0 ? pong(x - 1) : 1;
}

int pick_cycle(int x)
{
    if (pong(x))
        return 1;
    else if (pong(x))
        return 2;
    return 0;
}
