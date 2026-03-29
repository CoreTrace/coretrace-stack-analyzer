// SPDX-License-Identifier: Apache-2.0
struct Point
{
    int x;
    int y;
};

int read_struct_fully_initialized(void)
{
    struct Point p;
    p.x = 1;
    p.y = 2;
    return p.y;
}

// not contains: potential read of uninitialized local variable 'p'
