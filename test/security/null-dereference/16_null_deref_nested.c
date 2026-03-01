/**
 * 16 - NULL DEREF IN NESTED CONTROL FLOW (CWE-476)
 *
 * Compile: gcc -Wall -Wextra -g -fsanitize=address 16_null_deref_nested.c -o 16_test
 * Analyze: clang --analyze 16_null_deref_nested.c
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* 16a. malloc non verifie dans un nested if */
void vuln_nested_if_unchecked_malloc(int c1, int c2)
{
    if (c1)
    {
        if (c2)
        {
            int* arr = (int*)malloc(sizeof(int));
            arr[0] = 42;
            free(arr);
        }
    }
}

/* 16b. malloc non verifie dans un nested loop */
void vuln_nested_loop_unchecked_malloc(int n)
{
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j < n; ++j)
        {
            int* p = (int*)malloc(sizeof(int));
            p[0] = i + j;
            free(p);
        }
    }
}

/* 16c. dereference dans une branche prouvant ptr == NULL en nested if */
void vuln_nested_if_null_branch(int* ptr, int c1, int c2)
{
    if (c1)
    {
        if (c2)
        {
            if (ptr == NULL)
            {
                printf("val = %d\n", *ptr);
            }
        }
    }
}

int main(void)
{
    vuln_nested_if_unchecked_malloc(1, 1);
    vuln_nested_loop_unchecked_malloc(1);
    vuln_nested_if_null_branch(NULL, 1, 1);
    return 0;
}

// run_test expectations
// resource-model: models/resource-lifetime/generic.txt
// escape-model: models/stack-escape/generic.txt
// buffer-model: models/buffer-overflow/generic.txt

// at line 17, column 20
// [ !!Warn ] potential null pointer dereference on '<pointer>'
//             ↳ pointer comes from allocator return value and is dereferenced without a provable null-check

// at line 28, column 18
// [ !!Warn ] potential null pointer dereference on '<pointer>'
//             ↳ pointer comes from allocator return value and is dereferenced without a provable null-check

// at line 35, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'ptr' in function 'vuln_nested_if_null_branch' is never used to modify the pointed object
//             ↳ current type: int *ptr
//             ↳ suggested type: const int *ptr

// at line 39, column 38
// [!!!Error] potential null pointer dereference on '<pointer>'
//             ↳ control flow proves pointer is null on this branch before dereference
