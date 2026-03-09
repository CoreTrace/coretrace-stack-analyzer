struct RawBits
{
    unsigned first : 1;
    unsigned second : 1;
};

int bitfield_missing_init_should_warn(void)
{
    RawBits bits;
    return static_cast<int>(bits.second);
}

// at line 10, column 34
// [ !!Warn ] potential read of uninitialized local variable 'bits'
//          ↳ this load may execute before any definite initialization on all control-flow paths
