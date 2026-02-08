#include <stdio.h>
#define IS_ZERO(x) ((x) == 0)

int main(void)
{
    int num = 13;

    for (int i = 0; i < num; ++i)
    {
        for (int j = 0; j < num; ++j)
        {
            if (!!num)
            {
                if ((num))
                {
                    printf("Num is zero\n");
                }
                // at line 21, column 26
                // [!] unreachable else-if branch: condition is equivalent to a previous 'if' condition
                //     else branch implies previous condition is false
                else if ((num))
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
