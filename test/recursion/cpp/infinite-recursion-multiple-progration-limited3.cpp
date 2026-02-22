#include <iostream>

void foo();
void foobar();

void foobar()
{
    static int test = 10;

    test++;

    if (test == 5)
        return;
    foo();
}

void bar()
{
    foobar();
}

void foo(void)
{
    bar();
}

int main(void)
{
    foo();

    return 0;
}
