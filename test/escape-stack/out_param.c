void leak_out_param(char** out)
{
    char buf[10];
    // at line 7, column 10
    // [!!] stack pointer escape: address of variable 'buf' escapes this function
    //     stored through a non-local pointer (e.g. via an out-parameter; pointer may outlive this function)
    *out = buf; // fuite via paramètre de sortie
}

void safe_out_param(char** out)
{
    char* local = 0; // pointeur, mais pas de stack buffer derrière
    *out = local;    // pas une adresse de variable de stack
}
