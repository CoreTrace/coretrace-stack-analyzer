// SPDX-License-Identifier: Apache-2.0
//
// A second deterministic is_ready() (#157), as when two programs are analyzed together. Every
// definition of the symbol is deterministic: the callers of cross-tu-deterministic-use.c keep the
// fact.

int is_ready(int x)
{
    return x >= 4;
}
