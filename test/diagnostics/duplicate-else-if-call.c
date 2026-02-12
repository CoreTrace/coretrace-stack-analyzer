#include <string.h>

static int same_flag(const char* arg)
{
    return strcmp(arg, "--stack-limit") == 0;
}

int main(int argc, char** argv)
{
    if (argc < 2)
        return 0;

    const char* arg = argv[1];
    if (same_flag(arg))
    {
        return 1;
    }
    // at line 21, column 14
    // [ !!Warn ] unreachable else-if branch: condition is equivalent to a previous 'if' condition
    //          ↳ else branch implies previous condition is false
    else if (same_flag(arg))
    {
        return 2;
    }

    return 0;
}
