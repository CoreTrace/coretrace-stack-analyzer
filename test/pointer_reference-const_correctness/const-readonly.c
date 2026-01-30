#include <stdio.h>
#include <stdbool.h>

void increment(int* value)
{
    (*value)++;
}

void modify(int* ptr)
{
    *ptr = 42;
}

void process(int* data)
{
    modify(data); // assume modify(int *) changes pointee
}

void fill_array(int* arr, size_t n, bool condition)
{
    for (size_t i = 0; i < n; ++i)
    {
        if (condition)
            arr[i] = 0;
    }
}

// at line 26, column 0
// [!]ConstParameterNotModified.Pointer: parameter 'reg' in function 'read_volatile' is never used to modify the pointed object
//     current type: volatile int *reg
//     suggested type: const volatile int *reg
void read_volatile(volatile int* reg)
{
    int val = *reg;
    // use val
}

// at line 35, column 0
// [!]ConstParameterNotModified.Pointer: parameter 'value' in function 'log' is never used to modify the pointed object
//     current type: int *value
//     suggested type: const int *value
void log(int* value)
{
    printf("%d\n", *value); // read
    // but if printf were variadic mutating, conservative
}
