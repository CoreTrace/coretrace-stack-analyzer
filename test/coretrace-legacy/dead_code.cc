// SPDX-License-Identifier: Apache-2.0
int compute(int x)
{
    int y = x + 1;
    if (x > 0)
    {
        return x * 2;
    }
    return x * 3; // Code mort : jamais atteint car toutes les branches retournent avant.
}

int over_run_compute(int x)
{
    int y = x + 1;
    if (x > 0)
    {
        return x * 2; // Code mort : jamais atteint car toutes les branches retournent avant.
    }
    return x * 3;
}

int single_compute(void)
{
    int x = 5;

    if (x > 0)
    {
        return 2; // Code mort : jamais atteint car toutes les branches retournent avant.
    }
    return 3;
}

int main()
{
    int a = 5;
    int b = compute(a);
    a = -5;
    int c = over_run_compute(a);
    int d = single_compute();
    return b + c + d;
}

// at line 6, column 18
// [ !!Warn ] potential signed integer overflow in arithmetic operation

// at line 8, column 14
// [ !!Warn ] potential signed integer overflow in arithmetic operation

// at line 16, column 18
// [ !!Warn ] potential signed integer overflow in arithmetic operation

// at line 18, column 14
// [ !!Warn ] potential signed integer overflow in arithmetic operation
