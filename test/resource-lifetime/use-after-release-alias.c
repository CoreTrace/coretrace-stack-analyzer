// SPDX-License-Identifier: Apache-2.0
// resource-model: models/resource-lifetime/generic.txt
// A copy of a handle keeps naming the released memory; clearing or reassigning the owner, or
// comparing the pointer, reads no released memory.
#include <stdlib.h>

extern void consume(int* p);
extern int* other_ptr(void);

void alias_then_null(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    int* q = p;
    free(p);
    p = NULL; /* not a use */
    *q = 42;  /* use after release */
}

void alias_no_null(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    int* q = p;
    free(p);
    *q = 42;
}

int alias_read(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return 0;
    *p = 1;
    int* q = p;
    free(p);
    return *q;
}

void null_only(void)
{
    int* p = malloc(sizeof *p);
    free(p);
    p = NULL;
}

void reassigned_before_use(void)
{
    int* p = malloc(sizeof *p);
    free(p);
    p = malloc(sizeof *p);
    if (!p)
        return;
    *p = 1;
    free(p);
}

int compare_after_release(const int* other)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return 0;
    free(p);
    return p == other;
}

void pass_to_external(void)
{
    int* h = malloc(sizeof *h);
    if (!h)
        return;
    free(h);
    consume(h);
}

void use_after_external_then_release(void)
{
    int* s = malloc(sizeof *s);
    if (!s)
        return;
    consume(s); /* may keep or release it: unknown */
    free(s);    /* released for certain */
    *s = 1;     /* use after release */
}

void reassigned_from_unmodelled_callee(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    free(p);
    p = other_ptr(); /* not a use: the slot now holds another pointer */
    consume(p);
}

// at line 18, column 6
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 'q' is used after a release in this function

// at line 28, column 6
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 'q' is used after a release in this function

// at line 39, column 13
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 'q' is used after a release in this function

// at line 75, column 13
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 'h' is used after a release in this function

// at line 85, column 6
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 's' is used after a release in this function

// not contains: potential use-after-release: 'HeapAlloc' handle 'p' is used after a release in this function
