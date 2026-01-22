#include <unistd.h>

int consume(int n)
{
    // --- at line 9, column 5
    // [!] dynamic stack allocation detected for variable 'buf'
    //     allocated type: i8
    //     size of this allocation is not compile-time constant (VLA / variable alloca) and may lead to unbounded stack usage
    char buf[n];
    return buf[0];
}

int main(void)
{
    int n = (int)getpid();
    return consume(n);
}
