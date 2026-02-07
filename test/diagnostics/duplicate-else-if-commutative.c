int main(int argc, char* argv[])
{
    int num = argc - 1;

    if (0 == num)
    {
        return 0;
    }
    // at line 12, column 18
    // [!] unreachable else-if branch: condition is equivalent to a previous 'if' condition
    //     else branch implies previous condition is false
    else if (num == 0)
    {
        return 1;
    }

    return 2;
}
