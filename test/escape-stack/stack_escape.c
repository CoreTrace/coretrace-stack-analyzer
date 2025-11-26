// tests/stack_escape.c
char *g_ptr;
struct Holder {
    char *p;
};
struct Holder G;

typedef void (*cb_t)(char *);

char *ret_buf(void)
{
    char buf[10];
    return buf;
}

void store_global(void)
{
    char buf[10];
    g_ptr = buf;
}

void store_in_global_field(void)
{
    char buf[10];
    G.p = buf;
}

void leak_out_param(char **out)
{
    char buf[10];
    *out = buf;
}

void safe_out_param(char **out)
{
    char *local = 0;
    *out = local;
}

void use_callback(cb_t cb)
{
    char buf[10];
    cb(buf);
}

void sink(char *p);

void pass_to_sink(void)
{
    char buf[10];
    sink(buf);
}

void local_alias_only(void)
{
    char buf[10];
    char *p = buf;
    char **pp = &p;
    (void)pp;
}

int main(void)
{
    char *p;
    leak_out_param(&p);
    use_callback((cb_t)0);
    pass_to_sink();
    local_alias_only();
    store_global();
    store_in_global_field();
    return 0;
}
