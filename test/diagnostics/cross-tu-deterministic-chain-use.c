// SPDX-License-Identifier: Apache-2.0
//
// The caller of a chain over three files (#157). With cross-tu-deterministic-chain-mid.c and
// cross-tu-deterministic-chain-leaf.c, mid_ready() is deterministic and the else-if repeats the
// if. Without the leaf, mid_ready() calls an unknown function and nothing is reported.

int mid_ready(int x);

int pick_chain(int x)
{
    if (mid_ready(x))
        return 1;
    else if (mid_ready(x))
        return 2;
    return 0;
}
