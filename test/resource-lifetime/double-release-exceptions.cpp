// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;
extern void acquire_handle(handle_t* out) noexcept;
extern void release_handle(handle_t handle) noexcept;
extern void may_throw();

// resource-model: models/resource-lifetime/generic.txt
// The path through a catch that releases and returns does not reach the release after the try;
// a catch that releases and falls through does. A delete on each branch deletes once.

void catch_returns()
{
    handle_t returned;
    acquire_handle(&returned);
    try
    {
        may_throw();
    }
    catch (...)
    {
        release_handle(returned);
        return;
    }
    release_handle(returned);
}

void catch_falls_through()
{
    handle_t fallen;
    acquire_handle(&fallen);
    try
    {
        may_throw();
    }
    catch (...)
    {
        release_handle(fallen);
    }
    release_handle(fallen);
}

void delete_on_each_branch(int c)
{
    int* branchp = new int(1);
    if (c)
        delete branchp;
    else
        delete branchp;
}

// not contains: handle 'returned'
// not contains: handle 'branchp'

// at line 39, column 5
// [!!!Error] potential double release: 'GenericHandle' handle 'fallen' is released without a matching acquire in this function
