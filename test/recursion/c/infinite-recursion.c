#include <stdio.h>

// [!] recursive or mutually recursive function detected
// [!!!] unconditional self recursion detected (no base case)
//     this will eventually overflow the stack at runtime
void tutu(void)
{
    tutu();
}

int main(void)
{
    tutu();

    return 0;
}
