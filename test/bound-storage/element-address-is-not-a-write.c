// SPDX-License-Identifier: Apache-2.0
// Storing the address of an element in a pointer does not access the array, and C allows the
// address one past the end. Only the pointers are written here.
int end_pointer(void)
{
    char buf[10] = {0};
    const char* end = &buf[10];
    int sum = 0;
    for (const char* p = buf; p != end; ++p)
        sum += *p;
    return sum;
}

void address_in_loop(void)
{
    char buf[10];
    for (int i = 0; i <= 10; ++i)
    {
        char* p = &buf[i];
        if (i < 10)
            *p = 42;
    }
}

// strict-expectation-details: true

// not contains: buffer overflow
// not contains: negative index
