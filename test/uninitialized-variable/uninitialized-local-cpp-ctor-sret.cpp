#include <string>

int ctor_and_sret_should_not_warn(void)
{
    std::string input = "abc";
    std::string piece = input.substr(1);
    return static_cast<int>(piece.size());
}

// not contains: potential read of uninitialized local variable 'input'
// not contains: potential read of uninitialized local variable 'piece'
// not contains: local variable 'input' is never initialized
// not contains: local variable 'piece' is never initialized
// not contains: local variable 'ref.tmp' is never initialized
// not contains: local variable 'agg.tmp' is never initialized
