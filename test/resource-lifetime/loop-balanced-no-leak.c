// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;
extern void acquire_handle(handle_t* out);
extern handle_t acquire_ret(void);
extern void release_handle(handle_t handle);
extern int check(void);

// resource-model: models/resource-lifetime/generic.txt
void loop_balanced(int n)
{
    handle_t h;
    for (int i = 0; i < n; ++i)
    {
        acquire_handle(&h);
        release_handle(h);
    }
}

// not contains: potential resource leak
