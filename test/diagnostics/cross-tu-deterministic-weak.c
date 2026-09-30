// SPDX-License-Identifier: Apache-2.0
//
// A weak is_ready() (#157). The linker may retain another definition of the symbol instead, so it
// proves nothing about the one that runs.

__attribute__((weak)) int is_ready(int x)
{
    return x > 3;
}
