// SPDX-License-Identifier: Apache-2.0
// windows-skip: virtual callback escape modeling still produces a Windows-only false positive in this fixture.
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
