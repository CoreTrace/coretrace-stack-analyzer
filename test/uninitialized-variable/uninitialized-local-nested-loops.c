int read_nested_loops(int n, int m)
{
    int value;
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j < m; ++j)
        {
            value = i + j;
        }
    }
    return value;
}

// at line 11, column 12
// [!!] potential read of uninitialized local variable 'value'
//      this load may execute before any definite initialization on all control-flow paths
