int read_array_fully_initialized(void)
{
    int arr[2];
    arr[0] = 10;
    arr[1] = 20;
    return arr[1];
}

// not contains: potential read of uninitialized local variable 'arr'
