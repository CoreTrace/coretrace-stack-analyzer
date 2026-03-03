extern "C" void free(void*);

struct Holder
{
    void* ptr;
};

static void release_holder(Holder* holder)
{
    free(holder->ptr);
}

// resource-model: models/resource-lifetime/generic.txt
int resource_lifetime_summary_release_aggregate_local_no_incomplete(void)
{
    Holder holder = {nullptr};
    release_holder(&holder);
    return 0;
}

// not contains: inter-procedural resource analysis incomplete: handle 'holder'
// not contains: potential double release: 'HeapAlloc' handle 'holder'
