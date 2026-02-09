struct Pair
{
    int x;
    int y;
};

int read_struct_partial(void)
{
    struct Pair p;
    p.x = 7;
    return p.y;
}

// at line 11, column 14
// [!!] potential read of uninitialized local variable 'p'
//      this load may execute before any definite initialization on all control-flow paths
