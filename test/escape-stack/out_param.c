// case_out_param.c
void leak_out_param(char **out)
{
    char buf[10];
    *out = buf;   // fuite via paramètre de sortie
}

// case_out_param_safe.c
void safe_out_param(char **out)
{
    char *local = 0;  // pointeur, mais pas de stack buffer derrière
    *out = local;     // pas une adresse de variable de stack
}
