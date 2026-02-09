int read_uninitialized_branch(int cond)
{
    int x;

    if (cond)
    {
        x = 42;
    }
    // at line 12, column 12
    // [!!] potential read of uninitialized local variable 'x'
    //      this load may execute before any definite initialization on all control-flow paths
    return x;
}
