#include <stdio.h>

void toto(void)
{
    char test[100];
    int a = 5;
    int b = 10;
    int sum = a + b;
    test[0] = '\0'; // Initialize the string
    snprintf(test, sizeof(test), "Hello, World! %d %d %d\n", a, b, sum);
    printf("%s", test);
}

void tutu(void)
{
    static int counter = 0;
    counter++;
    if (counter == 5)
        return;
    tutu();
}

int main(void)
{
    int a = 5;
    int b = 10;
    int sum = a + b;
    const bool is_ok = false;

    if (is_ok) {
        char test[100];
        test[0] = '\0'; // Initialize the string
        snprintf(test, sizeof(test), "Hello, World! %d %d %d\n", a, b, sum);
    }

    tutu();

    return sum;
}
