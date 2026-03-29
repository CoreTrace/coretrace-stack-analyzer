// SPDX-License-Identifier: Apache-2.0
extern "C" void acquire_handle(void** out);
extern "C" void release_handle(void* handle);

// resource-model: models/resource-lifetime/generic.txt
class OwnerManualCleanup
{
  public:
    OwnerManualCleanup()
    {
        acquire_handle(&handle_);
    }

    ~OwnerManualCleanup() {}

    void cleanup()
    {
        release_handle(handle_);
    }

  private:
    void* handle_{};
};

int missing_destructor_release_cleanup_method_main()
{
    OwnerManualCleanup owner;
    owner.cleanup();
    return 0;
}

// not contains: resource acquired in constructor may leak: class 'OwnerManualCleanup' does not release 'GenericHandle'
