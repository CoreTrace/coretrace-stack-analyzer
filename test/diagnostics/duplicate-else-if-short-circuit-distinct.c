// SPDX-License-Identifier: Apache-2.0
#include <stdbool.h>

// not contains: unreachable else-if branch: condition is equivalent to a previous 'if' condition
int classify(bool a, bool b, bool c)
{
    if (a && b)
    {
        return 1;
    }
    else if (a && c)
    {
        return 2;
    }
    return 0;
}

int main(void)
{
    return classify(1, 0, 1);
}
