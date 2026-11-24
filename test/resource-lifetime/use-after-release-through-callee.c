// SPDX-License-Identifier: Apache-2.0
// resource-model: models/resource-lifetime/generic.txt
#include <stdlib.h>

// A call after a release is a use when the callee may need the memory alive: it writes or reads
// through the pointer (maybe on some paths only), hands it to unknown code, or passes it on. A
// callee that only keeps the pointer is not a use. Each live_ function makes the same call before
// the release: no diagnostic.

extern void ext_sink(int* p);
int* kept;

void writes(int* p)
{
    *p = 42;
}
int reads(const int* p)
{
    return *p;
}
void writes_if(int* p, int c)
{
    if (c)
        *p = 1;
}
void forwards_to_external(int* p)
{
    ext_sink(p);
}
void forwards_to_local(int* p)
{
    writes(p);
}
void keeps(int* p)
{
    kept = p;
}

void released_writes(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    free(p);
    writes(p);
}
int released_reads(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return 0;
    free(p);
    return reads(p);
}
void released_writes_if(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    free(p);
    writes_if(p, 1);
}
void released_forwards_to_external(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    free(p);
    forwards_to_external(p);
}
void released_forwards_to_local(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    free(p);
    forwards_to_local(p);
}
void released_indirect(void (*fp)(int*))
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    free(p);
    fp(p);
}
void released_keeps(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    free(p);
    keeps(p); /* only keeps the pointer: not a use */
}

void live_writes(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    writes(p);
    free(p);
}
int live_reads(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return 0;
    const int v = reads(p);
    free(p);
    return v;
}
void live_writes_if(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    writes_if(p, 1);
    free(p);
}
void live_forwards_to_local(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    forwards_to_local(p);
    free(p);
}
void live_indirect(void (*fp)(int*))
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    fp(p);
    free(p);
}

// A callee that releases the pointer is still a use when it also accesses it, on the same path
// or on another one. A callee that only releases it is no use: the second release is reported
// as a double release only.
void writes_then_frees(int* p)
{
    *p = 42;
    free(p);
}
void frees_or_writes(int* p, int c)
{
    if (c)
        free(p);
    else
        *p = 1;
}
void frees(int* p)
{
    free(p);
}

void released_writes_then_frees(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    free(p);
    writes_then_frees(p);
}
void released_frees_or_writes(int c)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    free(p);
    frees_or_writes(p, c);
}
void released_frees(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    free(p);
    frees(p); /* a double release, not a use */
}
void live_writes_then_frees(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    writes_then_frees(p);
}
void live_frees_or_writes(int c)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    frees_or_writes(p, c);
}
void live_frees(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    frees(p);
}

// Reported at the argument of each released_ call except released_keeps and released_frees;
// the strict count checks that those and the live_ functions report nothing else. The only
// other diagnostics are the double releases and the leak live_frees_or_writes may cause.

// at line 45, column 12
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 'p' is used after a release in this function

// at line 53, column 18
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 'p' is used after a release in this function

// at line 61, column 15
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 'p' is used after a release in this function

// at line 69, column 26
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 'p' is used after a release in this function

// at line 77, column 23
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 'p' is used after a release in this function

// at line 85, column 8
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 'p' is used after a release in this function

// at line 164, column 23
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 'p' is used after a release in this function

// at line 164, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'p' is released without a matching acquire in this function

// at line 172, column 21
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 'p' is used after a release in this function

// at line 172, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'p' is released without a matching acquire in this function

// at line 180, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'p' is released without a matching acquire in this function

// at line 191, column 14
// [ !!Warn ] potential resource leak: 'HeapAlloc' acquired in handle 'p' may leave the function without being released
