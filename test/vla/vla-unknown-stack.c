#include <unistd.h>

int consume(int n)
{
    char buf[n];
    return buf[0];
}

int main(void)
{
    int n = (int)getpid();
    return consume(n);
}

// at line 5, column 5
// [ !!Warn ] dynamic stack allocation detected for variable 'vla'
// ↳ allocated type: i8
// ↳ size of this allocation is not compile-time constant (VLA / variable alloca) and may lead to unbounded stack usage

// at line 5, column 5
// [ !!Warn ] user-controlled alloca size for variable 'vla'
// ↳ allocation performed via alloca/VLA; stack usage grows with runtime value
// ↳ size is unbounded at compile time
// ↳ size depends on user-controlled input (function argument or non-local value)