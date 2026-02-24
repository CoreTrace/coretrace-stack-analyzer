typedef struct P
{
    char* header;
    char* expanded;
    char* out;
    void* s;
} P;

extern void free(void* ptr);
extern int parse_file(P* p);

// resource-model: models/resource-lifetime/generic.txt
static int info_raw(P* p)
{
    if (!parse_file(p))
        return 0;

    free(p->expanded);
    free(p->out);
    return 1;
}

int fp_stb_like_struct_double_release(void* s)
{
    P p;
    p.s = s;
    return info_raw(&p);
}

// not contains: potential double release: 'HeapAlloc' handle 'p+16' is released without a matching acquire in this function
// not contains: potential double release: 'HeapAlloc' handle 'p+8' is released without a matching acquire in this function
