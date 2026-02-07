#include <stdio.h>

// not contains: [!] unreachable else-if branch: condition is equivalent to a previous 'if' condition
int main(int argc, char* argv[])
{
    int num = argc - 1;

    if (num == 0)
    {
        return 0;
    }

    if (num == 0)
    {
        return 1;
    }

    return 2;
}
