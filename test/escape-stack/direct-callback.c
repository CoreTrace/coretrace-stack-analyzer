// case_call_arg.c
void sink(char *p);

void pass_to_sink(void)
{
    char buf[10];
    sink(buf);   // le callee peut capturer le pointeur
}
