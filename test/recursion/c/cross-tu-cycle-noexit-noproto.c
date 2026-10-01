// SPDX-License-Identifier: Apache-2.0
//
// The other half of the cycle of cross-tu-cycle-noexit-a.c, calling ping() through a declaration
// without a prototype (#157). The call still reaches ping(), as within one file.

void ping();

void pong(int n)
{
    ping(n + 1);
}
