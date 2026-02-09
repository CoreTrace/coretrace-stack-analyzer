int read_initialized_ok(int cond)
{
    int x = 0;
    if (cond)
    {
        x = 7;
    }
    return x;
}

// not contains: potential read of uninitialized local variable
