// SPDX-License-Identifier: Apache-2.0
//
// Calls is_ready() through a declaration without a prototype (#157). With
// cross-tu-deterministic-def.c, it is deterministic and the else-if repeats the if, as within one
// file.

int is_ready();

int pick_noproto(int x)
{
    if (is_ready(x))
        return 1;
    else if (is_ready(x))
        return 2;
    return 0;
}
