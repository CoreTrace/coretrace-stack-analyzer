// SPDX-License-Identifier: Apache-2.0
// C++ input for the ownership fact collector: exceptional exits.
typedef void* handle_t;
extern void acquire_handle(handle_t* out);
extern void release_handle(handle_t h) noexcept;
extern void may_throw();
extern void never_throws() noexcept;

void call_may_throw()
{
    handle_t h;
    acquire_handle(&h);
    may_throw();
    release_handle(h);
}

void call_nothrow() noexcept
{
    handle_t h;
    acquire_handle(&h);
    never_throws();
    release_handle(h);
}

void invoke_with_catch()
{
    handle_t h;
    acquire_handle(&h);
    try
    {
        may_throw();
    }
    catch (...)
    {
    }
    release_handle(h);
}
