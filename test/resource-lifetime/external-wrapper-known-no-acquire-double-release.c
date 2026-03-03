typedef void* handle_t;

extern void release_handle(handle_t handle);

// resource-model: models/resource-lifetime/generic.txt
static void observe_no_acquire(handle_t* out)
{
    (void)out;
}

int external_wrapper_known_no_acquire_double_release(void)
{
    handle_t h;
    observe_no_acquire(&h);
    release_handle(h);
    return 0;
}

// at line 15, column 5
// [!!!Error] potential double release: 'GenericHandle' handle 'h' is released without a matching acquire in this function

// at line 15, column 20
// [ !!Warn ] potential read of uninitialized local variable 'h'
