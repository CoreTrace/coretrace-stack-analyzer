// case_global_struct.c
struct Holder {
    char *p;
};

struct Holder G;

void store_in_global_field(void)
{
    char buf[10];
    G.p = buf;   // fuite : G est global
}
