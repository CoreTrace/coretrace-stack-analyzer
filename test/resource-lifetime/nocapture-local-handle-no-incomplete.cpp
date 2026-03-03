typedef void* handle_t;
extern handle_t acquire_handle(void);
extern void release_handle(handle_t);

// External function that only reads the handle value without capturing it.
// The byval/nocapture-like lowering depends on the target; here we rely on
// the pointer being passed by value (not by address) so the local slot
// that holds it does not escape.
extern int inspect_handle(handle_t h);

// resource-model: models/resource-lifetime/generic.txt
int nocapture_local_handle_no_incomplete(void)
{
    handle_t h = acquire_handle();
    int status = inspect_handle(h);
    release_handle(h);
    return status;
}

// not contains: inter-procedural resource analysis incomplete
// not contains: potential double release
