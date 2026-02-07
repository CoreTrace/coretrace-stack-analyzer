void leak_out_param(char** out)
{
    char buf[10];
    // at line 7, column 10
    // [!!] stack pointer escape: address of variable 'buf' escapes this function
    //     stored through a non-local pointer (e.g. via an out-parameter; pointer may outlive this function)
    *out = buf; // leak via out-parameter
}

void safe_out_param(char** out)
{
    char* local = 0; // pointer, but no stack buffer behind it
    *out = local;    // not a stack variable address
}
