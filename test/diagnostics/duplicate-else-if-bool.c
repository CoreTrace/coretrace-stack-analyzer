#include <stdbool.h>

int main(int argc, char* argv[])
{
    bool hasFilter = argc > 1;

    if (hasFilter)
    {
        return 0;
    }
    // at line 14, column 14
    // [!] unreachable else-if branch: condition is equivalent to a previous 'if' condition
    //     else branch implies previous condition is false
    else if (hasFilter)
    {
        return 1;
    }

    return 2;
}
