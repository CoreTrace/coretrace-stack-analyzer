// tests/stack_escape_global.c
static char *g;

void set_global(void)
{
    char buf[10];
    g = buf;  // warning attendu: store_global
}

int main(void)
{
    set_global();
    return 0;
}
