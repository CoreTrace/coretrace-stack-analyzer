typedef void* handle_t;

extern void acquire_handle(handle_t* out);
extern void release_handle(handle_t handle);

// resource-model: models/resource-lifetime/generic.txt
int balanced_release(void)
{
    handle_t h;
    acquire_handle(&h);
    release_handle(h);
    return 0;
}

// not contains: potential resource leak: 'GenericHandle' acquired in handle 'h' is not released in this function
// not contains: potential double release: 'GenericHandle' handle 'h' is released without a matching acquire in this function
