typedef void* handle_t;

extern void acquire_handle(handle_t* out);
extern void release_handle(handle_t handle);

// resource-model: models/resource-lifetime/generic.txt
int leak_local_resource(void)
{
    handle_t h;
    acquire_handle(&h);
    return 0;
}

// at line 10, column 5
// [ !!Warn ] potential resource leak: 'GenericHandle' acquired in handle 'h' is not released in this function
