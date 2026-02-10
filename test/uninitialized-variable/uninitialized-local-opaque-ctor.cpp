struct OpaqueCtorType
{
    int value;
    OpaqueCtorType();
    ~OpaqueCtorType()
    {
        (void)value;
    }
};

int opaque_ctor_object_should_not_warn(void)
{
    OpaqueCtorType obj;
    return 0;
}

// not contains: potential read of uninitialized local variable 'obj'
