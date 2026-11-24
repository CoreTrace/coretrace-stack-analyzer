// SPDX-License-Identifier: Apache-2.0
// resource-model: models/resource-lifetime/generic.txt
#include <stdlib.h>

// Handing a released pointer over (to a global, an out parameter, a heap object, unknown code)
// does not make the memory live again, and an address computed from it (p + 1, p++, &t->b) still
// names the released memory. The use is the access through the pointer; the copy or the
// computation is not. Each live_ function does the same before the release, or without one: no
// diagnostic.

struct Holder
{
    int* q;
};
struct Pair
{
    int a;
    int b;
};
int* global;
extern void ext_keep(int* p);

void released_to_global(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    free(p);
    global = p;
    *p = 42;
}
void released_to_out_param(int** out)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    free(p);
    *out = p;
    *p = 42;
}
void released_to_heap_field(struct Holder* h)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    free(p);
    h->q = p;
    *p = 42;
}
void released_then_offset(void)
{
    int* p = malloc(2 * sizeof *p);
    if (!p)
        return;
    free(p);
    p = p + 1;
    *p = 42;
}
void released_then_increment(void)
{
    int* p = malloc(2 * sizeof *p);
    if (!p)
        return;
    free(p);
    p++;
    *p = 42;
}
void released_field_address(void)
{
    struct Pair* t = malloc(sizeof *t);
    if (!t)
        return;
    free(t);
    int* b = &t->b;
    *b = 42;
}
void released_to_unknown_code(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    free(p);
    ext_keep(p); /* a use: unknown code may read it */
    *p = 42;     /* still released: the callee cannot make it live again */
}

void live_to_global(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    global = p;
    *p = 42;
}
void live_to_out_param(int** out)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    *out = p;
    *p = 42;
}
void live_offset(void)
{
    int* p = malloc(2 * sizeof *p);
    if (!p)
        return;
    int* q = p + 1;
    *q = 42;
    free(p);
}
void live_field_address(void)
{
    struct Pair* t = malloc(sizeof *t);
    if (!t)
        return;
    int* b = &t->b;
    *b = 42;
    free(t);
}

// Reported at the access through the pointer (and at the call that hands it to unknown code),
// not at the copy or the address computation; the strict count checks that the live_ functions
// report nothing.

// at line 30, column 6
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 'p' is used after a release in this function

// at line 39, column 6
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 'p' is used after a release in this function

// at line 48, column 6
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 'p' is used after a release in this function

// at line 57, column 6
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 'p' is used after a release in this function

// at line 66, column 6
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 'p' is used after a release in this function

// at line 75, column 6
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 'b' is used after a release in this function

// at line 83, column 14
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 'p' is used after a release in this function

// at line 84, column 6
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 'p' is used after a release in this function
