int read_goto(int cond)
{
    int value;
    if (cond)
        goto done;
    value = 1;
done:
    return value;
}

// at line 8, column 12
// [!!] potential read of uninitialized local variable 'value'
//      this load may execute before any definite initialization on all control-flow paths
