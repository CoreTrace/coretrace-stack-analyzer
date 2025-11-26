// tests/stack_escape_return.c
char *ret_buf(void)
{
    char buf[10];
    return buf;   // warning attendu: return
}

int main(void)
{
    char *p = ret_buf();
    (void)p;
    return 0;
}
