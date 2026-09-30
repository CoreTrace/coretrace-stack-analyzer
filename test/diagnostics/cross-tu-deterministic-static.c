// SPDX-License-Identifier: Apache-2.0
//
// A static is_ready() (#157). It belongs to this file only: it gives nothing to the is_ready()
// that cross-tu-deterministic-use.c calls, and it does not count as one of its definitions.
// Within this file it is deterministic: local_pick() is reported.

static int is_ready(int x)
{
    return x > 3;
}

int local_pick(int x)
{
    if (is_ready(x))
        return 1;
    else if (is_ready(x))
        return 2;
    return 0;
}

// at line 16, column 14
// [ !!Warn ] unreachable else-if branch: condition is equivalent to a previous 'if' condition
//          ↳ else branch implies previous condition is false
