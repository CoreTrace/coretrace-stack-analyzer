typedef void* handle_t;
extern handle_t acquire_handle(void);
extern void release_handle(handle_t);

// resource-model: models/resource-lifetime/generic.txt
int local_non_escaping_no_incomplete(void)
{
    handle_t h = acquire_handle();
    // local_copy is never passed to any call; its address never escapes.
    // isNonEscapingLocalObject should prove this immediately.
    handle_t local_copy = h;
    release_handle(h);
    (void)local_copy;
    return 0;
}

// not contains: inter-procedural resource analysis incomplete
// not contains: potential double release
