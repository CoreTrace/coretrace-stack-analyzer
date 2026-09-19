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
