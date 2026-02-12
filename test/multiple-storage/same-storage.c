#include <string.h>

void foo(void)
{
    char buf[10];
    buf[0] = 'a';
    buf[1] = 'b';
    buf[2] = 'c';
    buf[2] = 'c';
}

int main(void)
{
    char src[20] = {0};
    foo();
    return 0;
}

// at line 5, column 1
// [ !Info! ] multiple stores to stack buffer 'buf' in this function (4 store instruction(s), 3 distinct index expression(s))
// [ !Info! ] stores use different index expressions; verify indices are correct and non-overlapping