struct CreateInfo
{
    int tag;
};

extern void* memset(void* dst, int ch, unsigned long n);

static void populate(CreateInfo& out)
{
    (void)memset(&out, 0, sizeof(CreateInfo));
}

int fp_create_info_memset(void)
{
    CreateInfo createInfo;
    populate(createInfo);
    return createInfo.tag;
}

// not contains: potential read of uninitialized local variable 'createInfo'
