// tests/stack_escape_global.c
static char* g;

void set_global(void)
{
    char buf[10];

    g = buf; // warning expected: store_global
}

int main(void)
{
    set_global();
    return 0;
}

// at line 6, column 1
// [ !!Warn ] local variable 'buf' is never initialized
//          ↳ declared without initializer and no definite write was found in this function

// at line 8, column 7
// [ !!Warn ] stack pointer escape: address of variable 'buf' escapes this function
//          ↳ stored into global variable 'g' (pointer may be used after the function returns)