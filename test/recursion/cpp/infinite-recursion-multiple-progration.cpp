#include <iostream>

void tutu();

// at line 13, column 5
// [ !Info! ] recursive or mutually recursive function detected
//
// at line 13, column 5
// [!!!Error] unconditional self recursion detected (no base case)
// ↳ this will eventually overflow the stack at runtime
void titi()
{
    tutu();
}

// at line 24, column 5
// [ !Info! ] recursive or mutually recursive function detected
//
// at line 24, column 5
// [!!!Error] unconditional self recursion detected (no base case)
// ↳ this will eventually overflow the stack at runtime
void tutu(void)
{
    titi();
}

int main(void)
{
    tutu();

    return 0;
}
