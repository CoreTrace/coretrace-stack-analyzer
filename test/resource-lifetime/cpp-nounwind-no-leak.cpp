// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;
extern void acquire_handle(handle_t* out) noexcept;
extern void release_handle(handle_t handle) noexcept;
extern void may_throw();
extern void never_throws() noexcept;

// resource-model: models/resource-lifetime/generic.txt
void nounwind_path() noexcept
{
    handle_t h;
    acquire_handle(&h);
    never_throws();
    release_handle(h);
}

// not contains: potential resource leak
