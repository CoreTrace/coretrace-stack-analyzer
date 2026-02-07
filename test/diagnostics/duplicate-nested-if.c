int main(int argc, char* argv[])
{
    int num = argc - 1;

    if (0 == num)
    {
        if (num == 0)
        {
            if (num == 0)
            {
                return 0;
            }
            // at line 16, column 26
            // [!] unreachable else-if branch: condition is equivalent to a previous 'if' condition
            //     else branch implies previous condition is false
            else if (num == 0)
            {
                return 0;
            }
        }
        return 0;
    }
    // at line 26, column 18
    // [!] unreachable else-if branch: condition is equivalent to a previous 'if' condition
    //     else branch implies previous condition is false
    else if (num == 0)
    {
        return 1;
    }

    return 2;
}