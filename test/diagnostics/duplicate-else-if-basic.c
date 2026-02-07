#include <stdio.h>

int main(int argc, char* argv[])
{
    int num = argc - 1;

    if (num == 0)
    {
        printf("Num is zero 1\n");
    }
    // at line 14, column 18
    // [!] unreachable else-if branch: condition is equivalent to a previous 'if' condition
    //     else branch implies previous condition is false
    else if (num == 0)
    {
        printf("Num is zero 2\n");
    }

    return 0;
}
