static void init_leaf(int* p)
{
    *p = 7;
}

static void init_mid(int* p)
{
    init_leaf(p);
}

int read_after_init_call_chain(void)
{
    int value;
    init_mid(&value);
    return value;
}

// not contains: potential read of uninitialized local variable 'value'
