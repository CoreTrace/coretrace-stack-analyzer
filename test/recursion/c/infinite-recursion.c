#include <stdio.h>

// at line 11, column 5
// [!] recursive or mutually recursive function detected

// at line 11, column 5
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
