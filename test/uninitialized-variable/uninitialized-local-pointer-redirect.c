static void redirect_ptr(int** out, int* target)
{
    *out = target;
}

int read_after_pointer_redirect(void)
{
    int uninit;
    int init = 42;
    int* ptr = &uninit;
    redirect_ptr(&ptr, &init);
    return *ptr;
}

// not contains: potential read of uninitialized local variable 'uninit'
