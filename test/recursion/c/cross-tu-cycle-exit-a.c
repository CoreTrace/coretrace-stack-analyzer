// SPDX-License-Identifier: Apache-2.0
//
// ping() and pong() call each other across two files (#157), with cross-tu-cycle-exit-b.c. ping()
// returns when n <= 0: both are recursive, and neither is reported as never returning. Their max
// stack, and that of enter_cycle(), is unknown, with a lower bound, as within one file.

int pong(int n);

int ping(int n)
{
    return n <= 0 ? 0 : pong(n - 1);
}
