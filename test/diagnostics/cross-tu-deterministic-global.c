// SPDX-License-Identifier: Apache-2.0
//
// An is_ready() that reads a mutable global (#157): it is not deterministic, alone or beside the
// deterministic definition of cross-tu-deterministic-def.c. A symbol is deterministic only if
// every definition is.

int ready_threshold = 3;

int is_ready(int x)
{
    return x > ready_threshold;
}
