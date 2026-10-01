// SPDX-License-Identifier: Apache-2.0
//
// The other half of the cycle of cross-tu-cycle-noexit-a.c (#157).

void ping(int n);

void pong(int n)
{
    ping(n + 1);
}
