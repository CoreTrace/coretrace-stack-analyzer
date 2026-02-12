int read_cpp_ref(int cond, int& out)
{
    int value;
    if (cond)
    {
        value = 42;
    }
    out = value;
    return out;
}

// at line 8, column 11
// [ !!Warn ] potential read of uninitialized local variable 'value'
// ↳ this load may execute before any definite initialization on all control-flow paths
