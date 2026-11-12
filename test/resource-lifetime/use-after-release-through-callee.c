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

// Reported at the argument of each released_ call except released_keeps; the strict count
// checks that released_keeps and the live_ functions report nothing.

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
