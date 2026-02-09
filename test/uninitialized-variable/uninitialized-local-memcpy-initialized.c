int memcpy_reads_initialized_source(void)
{
    int src = 7;
    int dst = 0;
    __builtin_memcpy(&dst, &src, sizeof(src));
    return dst;
}

// not contains: potential read of uninitialized local variable 'src'
