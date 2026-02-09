int read_array_elem(void)
{
    int arr[4];
    return arr[2];
}

// at line 4, column 12
// [!!] potential read of uninitialized local variable 'arr'
//      this load may execute before any definite initialization on all control-flow paths
