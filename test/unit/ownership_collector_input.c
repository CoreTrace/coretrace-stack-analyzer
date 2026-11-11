// SPDX-License-Identifier: Apache-2.0
// Input for the ownership fact collector unit tests.
typedef void* handle_t;
extern void acquire_handle(handle_t* out);
extern handle_t acquire_ret(void);
extern void release_handle(handle_t h);
extern void unknown_sink(handle_t h);
extern void unknown_out(handle_t* p);
extern int check(void);

int early_return(void)
{
    handle_t h;
    acquire_handle(&h);
    if (check())
        return -1;
    release_handle(h);
    return 0;
}

void alias_keep(void)
{
    handle_t h;
    handle_t saved;
    acquire_handle(&h);
    saved = h;
    acquire_handle(&h);
    release_handle(saved);
    release_handle(h);
}

handle_t select_return(int c)
{
    handle_t h = acquire_ret();
    return c ? h : (handle_t)0;
}

void unknown_call(void)
{
    handle_t h;
    acquire_handle(&h);
    unknown_sink(h);
    unknown_out(&h);
}

/* Which uses of a pointer are Use events. */
int use_kinds(char* other)
{
    char* p = acquire_ret();
    char* q = p;    /* copy into a slot: not a use */
    if (p == other) /* comparison: not a use */
        return 0;
    *p = 1;                    /* store through the pointer: a use */
    __builtin_memset(q, 0, 1); /* memory intrinsic: a use */
    unknown_sink(p);           /* external callee: a use (conservative) */
    release_handle(p);         /* the released argument: not a use */
    p = 0;                     /* reassignment: not a use */
    return 1;
}

char* use_return(void)
{
    char* p = acquire_ret();
    release_handle(p);
    return p; /* the pointer leaves the function: a use */
}
