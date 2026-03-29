// SPDX-License-Identifier: Apache-2.0
extern "C" void acquire_handle(void** out);
extern "C" void release_handle(void* handle);

// resource-model: models/resource-lifetime/generic.txt
class OwnerIndirect
{
  public:
    OwnerIndirect()
    {
        init_level_1();
    }

    ~OwnerIndirect() {}

  private:
    void init_level_1()
    {
        init_level_2();
    }

    void init_level_2()
    {
        acquire_handle(&handle_);
    }

    void* handle_{};
};

int missing_destructor_release_indirect_main()
{
    OwnerIndirect owner;
    (void)owner;
    return 0;
}

// at line 13, column 22
// [ !!Warn ] resource acquired in constructor may leak: class 'OwnerIndirect' does not release 'GenericHandle' field 'this+0' in destructor
