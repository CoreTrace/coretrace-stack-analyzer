#define IS_ZERO(x) ((x) == 0)

int main(int argc, char* argv[])
{
    int num = argc - 1;

    for (int i = 0; i < 1; ++i)
    {
        if (IS_ZERO(num))
        {
            num += 1;
        }
        else
        {
            // at line 18, column 17
            // [!] unreachable else-if branch: condition is equivalent to a previous 'if' condition
            //     else branch implies previous condition is false
            if (IS_ZERO(num))
            {
                return 1;
            }
        }
    }

    return 0;
}
