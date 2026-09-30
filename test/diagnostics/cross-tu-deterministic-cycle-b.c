// SPDX-License-Identifier: Apache-2.0
//
// The other half of the cycle of cross-tu-deterministic-cycle-a.c (#157).

int ping(int x);

int pong(int x)
{
    return x > 0 ? ping(x - 1) : 0;
}
