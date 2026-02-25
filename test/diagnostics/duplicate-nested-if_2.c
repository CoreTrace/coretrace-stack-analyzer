int main(int argc, char* argv[])
{
    int num = argc - 1;

    if (0 == num)
    {
        if (num != 0)
        {
            if (num == 0)
            {
                return 0;
            }
            else if (num == 0)
            {
                return 0;
            }
        }
        return 0;
    }
    else if (num == 0)
    {
        return 1;
    }

    return 2;
}

// at line 13, column 26
// [ !!Warn ] unreachable else-if branch: condition is equivalent to a previous 'if' condition
//             ↳ else branch implies previous condition is false

// at line 20, column 18
// [ !!Warn ] unreachable else-if branch: condition is equivalent to a previous 'if' condition
//             ↳ else branch implies previous condition is false
