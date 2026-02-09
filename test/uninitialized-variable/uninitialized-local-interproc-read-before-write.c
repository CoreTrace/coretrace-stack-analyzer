static int read_value(const int* p)
{
    return *p;
}

int read_via_call_before_init(void)
{
    int value;
    return read_value(&value);
}

// at line 9, column 12
// [!!] potential read of uninitialized local variable 'value'
//      this call may read the value before any definite initialization in 'read_value'
