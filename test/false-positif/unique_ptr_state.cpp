static void make_unique_sink(char* p)
{
    (void)p;
}

static void unique_ptr_sink(char* p)
{
    (void)p;
}

static void transition(void)
{
    char buf[8];
    make_unique_sink(buf);
    unique_ptr_sink(buf);
}

int main(void)
{
    transition();
    return 0;
}

// not contains: stack pointer escape
