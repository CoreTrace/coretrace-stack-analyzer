extern "C" void acquire_handle(void** out);
extern "C" void release_handle(void* handle);

// resource-model: models/resource-lifetime/generic.txt
class OwnerIndirectBalanced
{
  public:
    OwnerIndirectBalanced()
    {
        init_level_1();
    }

    ~OwnerIndirectBalanced()
    {
        shutdown_level_1();
    }

  private:
    void init_level_1()
    {
        init_level_2();
    }

    void init_level_2()
    {
        acquire_handle(&handle_);
    }

    void shutdown_level_1()
    {
        shutdown_level_2();
    }

    void shutdown_level_2()
    {
        release_handle(handle_);
    }

    void* handle_{};
};

int missing_destructor_release_indirect_balanced_main()
{
    OwnerIndirectBalanced owner;
    (void)owner;
    return 0;
}

// not contains: resource acquired in constructor may leak: class 'OwnerIndirectBalanced' does not release 'GenericHandle'
