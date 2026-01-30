#include <alloca.h>
#include <stddef.h>

void boom(size_t n)
{
    // at line 12, column 23
    // [!!] user-controlled alloca size for variable 'p'
    //     allocation performed via alloca/VLA; stack usage grows with runtime value
    //     size is unbounded at compile time
    //     function is infinitely recursive; this alloca runs at every frame and guarantees stack overflow
    //     size depends on user-controlled input (function argument or non-local value)
    char* p = (char*)alloca(n);
    boom(n);
}

int main(void)
{
    boom(32);
    return 0;
}
