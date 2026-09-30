// SPDX-License-Identifier: Apache-2.0
//
// Deterministic functions (#157), for the callers of cross-tu-deterministic-use.c and
// cross-tu-deterministic-noproto-use.c. is_ready_or_abort() calls abort(), which never returns:
// it stays deterministic, as within one file.

#include <stdlib.h>

int is_ready(int x)
{
    return x > 3;
}

int is_ready_or_abort(int x)
{
    if (x < 0)
        abort();
    return x > 3;
}
