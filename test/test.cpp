// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include <iostream>

namespace demo_symbols
{
struct Sample
{
    int value;
};

int scalar_parameters(int value, double scale, char tag)
{
    if (value > 0 && scale > 0.0 && tag != '\0')
        return 1;
    return 0;
}

long pointer_parameters(const int* value, const char* label, bool enabled)
{
    if (!value || !label || !enabled)
        return 0;
    return (*value > 0 && label[0] != '\0') ? 1L : 0L;
}

double reference_parameters(const Sample& sample, float ratio, unsigned long count)
{
    if (sample.value > 0 && ratio > 0.0f && count > 0)
        return 1.0;
    return 0.0;
}

int overloaded(int value)
{
    return value == 0 ? 0 : 1;
}

int overloaded(int lhs, int rhs)
{
    return lhs < rhs ? 1 : 0;
}
} // namespace demo_symbols

void toto(void)
{
    char test[100];
    int a = 5;
    int b = 10;
    int sum = a + b;
    test[0] = '\0'; // Initialize the string
    snprintf(test, sizeof(test), "Hello, World! %d %d %d\n", a, b, sum);
    printf("%s", test);
    std::cout << test;
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
    demo_symbols::Sample sample{sum};

    if (is_ok)
    {
        char test[100];
        test[0] = '\0'; // Initialize the string
        snprintf(test, sizeof(test), "Hello, World! %d %d %d\n", a, b, sum);
    }

    tutu();

    sum += demo_symbols::scalar_parameters(a, 2.5, 'x');
    sum += static_cast<int>(demo_symbols::pointer_parameters(&sum, "label", true));
    sum += static_cast<int>(demo_symbols::reference_parameters(sample, 1.5f, 3UL));
    sum += demo_symbols::overloaded(sum);
    sum += demo_symbols::overloaded(a, b);

    return sum;
}
