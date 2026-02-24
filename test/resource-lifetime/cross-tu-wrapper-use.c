typedef void* handle_t;

typedef struct CreateProps
{
    handle_t* out;
} CreateProps;

extern void create_wrapper_cross_tu(const CreateProps* props);
extern void release_handle(handle_t handle);

int cross_tu_wrapper_use(void)
{
    handle_t h = (handle_t)0;
    CreateProps props = {&h};
    create_wrapper_cross_tu(&props);
    release_handle(h);
    return 0;
}
