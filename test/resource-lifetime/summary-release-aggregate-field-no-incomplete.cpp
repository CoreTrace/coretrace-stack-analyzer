extern "C" void* malloc(unsigned long);
extern "C" void free(void*);

struct Container
{
    void* buffer;
    int size;
};

static void destroy_container(Container* c)
{
    free(c->buffer);
}

// resource-model: models/resource-lifetime/generic.txt
int aggregate_local_from_summary_no_incomplete(void)
{
    Container c;
    c.buffer = malloc(64);
    c.size = 64;
    destroy_container(&c);
    return 0;
}

// not contains: inter-procedural resource analysis incomplete: handle 'c'
// not contains: potential double release
