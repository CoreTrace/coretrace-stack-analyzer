int read_uninitialized_basic(void)
{
    int value;
    // at line 7, column 12
    // [!!] potential read of uninitialized local variable 'value'
    //      this load may execute before any definite initialization on all control-flow paths
    return value;
}
