struct Holder
{
    char* p;
};

struct Holder G;

void store_in_global_field(void)
{
    char buf[10];
    // at line 14, column 9
    // [!!] stack pointer escape: address of variable 'buf' escapes this function
    //     stored into global variable 'G' (pointer may be used after the function returns)
    G.p = buf; // leak : G is global
}
