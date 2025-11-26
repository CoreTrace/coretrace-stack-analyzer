#include <stdio.h>

int main(void)
{
    int n;
    if (scanf("%d", &n) != 1)
        return 1;

    char buf[n];  // VLA aussi

    return 0;
}
