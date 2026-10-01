// SPDX-License-Identifier: Apache-2.0
//
// The other half of the cycle of cross-tu-cycle-exit-a.c (#157), and a caller of the cycle.

int ping(int n);

int pong(int n)
{
    return ping(n);
}

int enter_cycle(int n)
{
    return ping(n);
}
