#include <string>

struct Base
{
    virtual ~Base() = default;

    void emit() const
    {
        log("INFO");
    }

    virtual void log(const std::string& message) const = 0;
};

void call_emit(const Base& base)
{
    base.emit();
}

// not contains: stack pointer escape: address of variable 'ref.tmp' escapes this function
