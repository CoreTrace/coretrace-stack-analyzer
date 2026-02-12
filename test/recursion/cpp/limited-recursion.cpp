#include <iostream>

// at line 9, column 12
// [!] recursive or mutually recursive function detected
void tutu(void)
{
    static int counter = 0;

    counter++;
    if (counter == 5)
        return;
    tutu();
}

int main(void)
{
    tutu();

    return 0;
}
