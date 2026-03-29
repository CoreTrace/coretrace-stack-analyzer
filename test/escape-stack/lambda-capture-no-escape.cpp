// SPDX-License-Identifier: Apache-2.0
// Test: lambda capturing local variables by reference should NOT trigger
// StackPointerEscape, because the closure lives on the same stack frame.

void use_int(int);

void lambda_capture_by_ref(int cond)
{
    int x = 0;
    int y = 0;

    auto update = [&](int val)
    {
        if (val > 0)
            x = val;
        else
            y = -val;
    };

    update(cond);
    use_int(x + y);
}

int main(void)
{
    lambda_capture_by_ref(42);
    return 0;
}

// not contains: stack pointer escape
