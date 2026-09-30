// SPDX-License-Identifier: Apache-2.0
//
// The end of a chain over three files (#157): cross-tu-deterministic-chain-use.c calls
// mid_ready(), which calls leaf_ready().

int leaf_ready(int x)
{
    return x > 3;
}
