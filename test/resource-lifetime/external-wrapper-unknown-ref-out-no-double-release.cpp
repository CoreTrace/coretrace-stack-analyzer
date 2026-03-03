extern "C" void release_handle(void* handle);

using handle_t = void*;

struct CreatePropsRef
{
    handle_t& out;
};

extern void create_wrapper_unknown_ref(const CreatePropsRef& props);

// resource-model: models/resource-lifetime/generic.txt
int external_wrapper_unknown_ref_out_no_double_release()
{
    handle_t h;
    CreatePropsRef props{h};
    create_wrapper_unknown_ref(props);
    release_handle(h);
    return 0;
}

// not contains: potential double release: 'GenericHandle' handle 'h' is released without a matching acquire in this function

// at line 18, column 20
// [ !!Warn ] potential read of uninitialized local variable 'h'

// at line 18, column 5
// [ !!Warn ] inter-procedural resource analysis incomplete: handle 'h' may be acquired by an unmodeled/external callee before release
