// SPDX-License-Identifier: Apache-2.0
//
// A static pong() (#157). It belongs to this file only: the calls of cross-tu-cycle-exit-a.c to
// pong() still reach the one of cross-tu-cycle-exit-b.c, and local_pong() is on no cycle.

static int pong(int n)
{
    return n;
}

int local_pong(int n)
{
    return pong(n);
}
