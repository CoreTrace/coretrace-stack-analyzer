extern "C" void acquire_handle(void** out);
extern "C" void release_handle(void* handle);

// resource-model: models/resource-lifetime/generic.txt
class Owner
{
  public:
    Owner()
    {
        acquire_handle(&handle_);
    }

    ~Owner() {}

  private:
    void* handle_{};
};

int main()
{
    Owner owner;
    (void)owner;
    return 0;
}

// at line 13, column 14
// [ !!Warn ] resource acquired in constructor may leak: class 'Owner' does not release 'GenericHandle' field 'this+0' in destructor
