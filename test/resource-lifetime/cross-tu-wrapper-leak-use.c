typedef void* handle_t;

typedef struct CreateProps
{
    handle_t* out;
} CreateProps;

extern void create_wrapper_cross_tu(const CreateProps* props);

int cross_tu_wrapper_leak_use(void)
{
    handle_t h = (handle_t)0;
    CreateProps props = {&h};
    create_wrapper_cross_tu(&props);
    return h != (handle_t)0;
}
