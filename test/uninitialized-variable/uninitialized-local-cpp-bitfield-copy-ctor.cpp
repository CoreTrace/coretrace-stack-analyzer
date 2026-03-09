struct ResolvedLike
{
    unsigned line = 0;
    unsigned column = 0;
    unsigned hasLocation : 1 = 0;
    unsigned reserved : 31 = 0;
};

static ResolvedLike makeResolvedLike(void)
{
    ResolvedLike loc;
    return loc;
}

int bitfield_copy_ctor_should_not_warn(void)
{
    const ResolvedLike loc = makeResolvedLike();
    return static_cast<int>(loc.line + loc.column);
}

// not contains: potential read of uninitialized local variable 'loc'
