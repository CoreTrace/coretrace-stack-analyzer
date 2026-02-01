#include <alloca.h>
#include <stddef.h>

// [!] recursive or mutually recursive function detected

// [!!!] unconditional self recursion detected (no base case)
//     this will eventually overflow the stack at runtime
void boom(size_t n)
{
    // at line 21, column 22
    // [!] dynamic stack allocation detected for variable 'p'
    //     allocated type: i8
    //     size of this allocation is not compile-time constant (VLA / variable alloca) and may lead to unbounded stack usage

    // at line 21, column 22
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
