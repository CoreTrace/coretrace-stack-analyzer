// SPDX-License-Identifier: Apache-2.0
//
// The middle of a chain over three files (#157). mid_ready() is deterministic only if
// leaf_ready(), defined in cross-tu-deterministic-chain-leaf.c, is.

int leaf_ready(int x);

int mid_ready(int x)
{
    return leaf_ready(x) && x < 100;
}
