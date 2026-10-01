// SPDX-License-Identifier: Apache-2.0
//
// ping() and pong() call each other across two files (#157), with cross-tu-cycle-noexit-b.c. No
// path leaves the cycle: as within one file, both are recursive and never return. Alone, ping()
// calls an unknown function and nothing is reported.

void pong(int n);

void ping(int n)
{
    pong(n);
}
