#include <stdio.h>

typedef void (*FuncPtr)(int);

void myFunction(int x)
{
    printf("Value: %d\n", x);
}

void anotherFunction(double y)
{
    printf("Double value: %f\n", y);
}

int main()
{
    FuncPtr fptr = (FuncPtr)anotherFunction; // Mauvaise conversion de type
    fptr(42); // Appel incorrect : un int est interprété comme un double
    return 0;
}
