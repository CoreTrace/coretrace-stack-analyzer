static const int* g_ptr = nullptr;

struct Base
{
    virtual ~Base() = default;

    void emit() const
    {
        int local = 42;
        log(local);
    }

    virtual void log(const int& value) const = 0;
};

struct CapturingLogger final : Base
{
    void log(const int& value) const override
    {
        g_ptr = &value;
    }
};

void drive_capture()
{
    CapturingLogger logger;
    logger.emit();
}

// at line 10, column 9
// [ !!Warn ] stack pointer escape: address of variable 'local' escapes this function
//          ↳ address passed as argument to an indirect call (callback may capture the pointer beyond this function)
