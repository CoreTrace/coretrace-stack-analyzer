// SPDX-License-Identifier: Apache-2.0
// Driven by check_ownership_cross_tu(): the callees defined in cross-tu-use-then-release-def.c
// release their argument. The call after a release is a use when the callee also accesses the
// argument (lines 18 and 26), not when it only releases it. The live_ functions report no use.
#include <stdlib.h>

extern void writes_then_frees_cross_tu(int* p);
extern void frees_or_writes_cross_tu(int* p, int c);
extern void frees_cross_tu(int* p);

// strict-diagnostic-count: false
void released_writes_then_frees(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    free(p);
    writes_then_frees_cross_tu(p);
}
void released_frees_or_writes(int c)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    free(p);
    frees_or_writes_cross_tu(p, c);
}
void released_frees(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    free(p);
    frees_cross_tu(p); /* a double release, not a use */
}
void live_writes_then_frees(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    writes_then_frees_cross_tu(p);
}
void live_frees_or_writes(int c)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    frees_or_writes_cross_tu(p, c);
}
void live_frees(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    frees_cross_tu(p);
}
