int read_switch(int tag)
{
    int value;
    switch (tag)
    {
    case 0:
        value = 10;
        break;
    case 1:
        break;
    default:
        value = 20;
        break;
    }
    return value;
}

// at line 15, column 12
// [ !!Warn ] potential read of uninitialized local variable 'value'
// ↳ this load may execute before any definite initialization on all control-flow paths
