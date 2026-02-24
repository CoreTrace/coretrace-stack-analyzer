typedef void* handle_t;

extern void acquire_handle(handle_t* out);
extern void release_handle(handle_t handle);

// resource-model: models/resource-lifetime/generic.txt
typedef struct CreateProps
{
    int tag;
    handle_t* out;
} CreateProps;

static void create_impl(const CreateProps* props)
{
    acquire_handle(props->out);
}

static void create_wrapper(const CreateProps* props)
{
    create_impl(props);
}

int interproc_wrapper_chain_balanced(void)
{
    handle_t h;
    CreateProps props = {0, &h};
    create_wrapper(&props);
    release_handle(h);
    return 0;
}

// not contains: potential resource leak: 'GenericHandle' acquired in handle 'h' is not released in this function
// not contains: potential double release: 'GenericHandle' handle 'h' is released without a matching acquire in this function
