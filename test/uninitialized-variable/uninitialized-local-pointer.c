int read_pointer_target(int cond)
{
    int fallback = 0;
    int* ptr;
    if (cond)
        ptr = &fallback;
    return *ptr;
}

// at line 7, column 13
// [!!] potential read of uninitialized local variable 'ptr'
//      this load may execute before any definite initialization on all control-flow paths
