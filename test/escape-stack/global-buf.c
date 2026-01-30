// tests/stack_escape_global.c
static char* g;

void set_global(void)
{
    char buf[10];
    // at line 10, column 7
    // [!!] stack pointer escape: address of variable 'buf' escapes this function
    //     stored into global variable 'g' (pointer may be used after the function returns)
    g = buf; // warning attendu: store_global
}

int main(void)
{
    set_global();
    return 0;
}
