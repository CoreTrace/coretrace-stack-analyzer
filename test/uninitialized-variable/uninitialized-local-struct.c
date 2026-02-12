struct Pair
{
    int x;
    int y;
};

int read_struct_field(void)
{
    struct Pair p;
    return p.x;
}

// at line 10, column 14
// [ !!Warn ] potential read of uninitialized local variable 'p'
// ↳ this load may execute before any definite initialization on all control-flow paths
