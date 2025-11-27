#include <stdio.h>

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
