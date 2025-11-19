#include <stdio.h>

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
