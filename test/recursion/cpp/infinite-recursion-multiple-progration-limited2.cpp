#include <iostream>

void foo();
void foobar();

void foobar()
{
    static int test = 0;

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

// at line 10, column 9
// [ !Info! ] recursive or mutually recursive function detected

// at line 24, column 5
// [ !Info! ] recursive or mutually recursive function detected

// at line 19, column 5
// [ !Info! ] recursive or mutually recursive function detected
