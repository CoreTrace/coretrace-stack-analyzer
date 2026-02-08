#include <stdio.h>
#include <stdbool.h>

int main(int argc, char* argv[])
{
    int num = argc;
    bool flag = (argc & 1) != 0;

    for (int i = 0; i < num; ++i)
    {
        for (int j = 0; j < num; ++j)
        {
            if (!!num)
            {
                if (flag)
                {
                    printf("Num is zero\n");
                }
                // at line 22, column 26
                // [!] unreachable else-if branch: condition is equivalent to a previous 'if' condition
                //     else branch implies previous condition is false
                else if (flag)
                {
                    printf("No functions matched filters for: %d\n", num);
                }
                else
                {
                    return 1;
                }
            }
        }
    }
    return 0;
}
