// SPDX-License-Identifier: Apache-2.0
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <stdint.h>

// at line 11, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'values' in function 'print_sum' is never used to modify the pointed object
//          ↳ current type: int *values
//          ↳ suggested type: const int *values
void print_sum(int* values, size_t count)
{
    int sum = 0;
    for (size_t i = 0; i < count; ++i)
    {
        sum += values[i];
    }
    printf("%d\n", sum);
}

void copy_data(const char* source, char* const dest, size_t len)
{
    memcpy(dest, source, len);
}

void read_data(char* const buffer)
{
    printf("%s\n", buffer);
}

// at line 40, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'a' in function 'get_max' is never used to modify the pointed object
//          ↳ current type: int *a
//          ↳ suggested type: const int *a

// at line 40, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'b' in function 'get_max' is never used to modify the pointed object
//          ↳ current type: int *b
//          ↳ suggested type: const int *b
int get_max(int* a, int* b)
{
    return (*a > *b) ? *a : *b;
}

struct Point
{
    int x, y;
};

// at line 59, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p1' in function 'distance' is never used to modify the pointed object
//          ↳ current type: Point *p1
//          ↳ suggested type: const Point *p1

// at line 59, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p2' in function 'distance' is never used to modify the pointed object
//          ↳ current type: Point *p2
//          ↳ suggested type: const Point *p2
int distance(struct Point* p1, struct Point* p2)
{
    return abs(p1->x - p2->x) + abs(p1->y - p2->y);
}

int distancePatched(const struct Point* p1, const struct Point* p2)
{
    int64_t dx = (int64_t)p1->x - (int64_t)p2->x;
    int64_t dy = (int64_t)p1->y - (int64_t)p2->y;

    uint64_t manhattan = (uint64_t)llabs(dx) + (uint64_t)llabs(dy);
    if (manhattan > (uint64_t)INT_MAX)
        return INT_MAX; // policy: saturation

    return (int)manhattan;
}

// at line 61, column 31
// [ !!Warn ] potential signed integer overflow in arithmetic operation
//          ↳ operation: add
//          ↳ result is returned without a provable non-overflow bound
