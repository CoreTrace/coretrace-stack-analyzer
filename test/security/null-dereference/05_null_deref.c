// SPDX-License-Identifier: Apache-2.0
/**
 * 05 - NULL POINTER DEREFERENCE (CWE-476)
 *
 * Compile: gcc -Wall -Wextra -g -fsanitize=address 05_null_deref.c -o 05_test
 * Analyze: clang --analyze 05_null_deref.c
 */

#include <stdio.h>
#include <stdlib.h>

/* 5a. malloc sans vérification */
void vuln_null_deref(size_t n)
{
    int* arr = (int*)malloc(n * sizeof(int));
    arr[0] = 42; /* CWE-476: arr peut être NULL si malloc échoue */
    free(arr);
}

/* 5b. Déréférencement dans la branche NULL (logique inversée) */
void vuln_null_deref_logic(int* ptr)
{
    if (ptr == NULL)
    {
        printf("val = %d\n", *ptr); /* CWE-476: déref garantie sur NULL */
    }
}

/* 5c. NULL après free puis réutilisation */
void vuln_null_after_free(void)
{
    int* p = (int*)malloc(sizeof(int));
    if (!p)
        return;
    *p = 10;
    free(p);
    p = NULL;
    printf("val = %d\n", *p); /* CWE-476: déref de NULL explicite */
}

int main(void)
{
    printf("=== 05: NULL Pointer Dereference Tests ===\n");
    vuln_null_deref(0); /* malloc(0) peut retourner NULL */
    vuln_null_deref_logic(NULL);
    return 0;
}

// run_test expectations
// resource-model: models/resource-lifetime/generic.txt
// escape-model: models/stack-escape/generic.txt
// buffer-model: models/buffer-overflow/generic.txt

// at line 13, column 23
// [ !!Warn ] potential integer overflow in size computation before 'malloc'
// ↳ operation: mul
// ↳ overflowed size may under-allocate memory or make bounds checks unsound

// at line 14, column 12
// [ !!Warn ] potential null pointer dereference on '<pointer>'
// ↳ pointer comes from allocator return value and is dereferenced without a provable null-check

// at line 21, column 30
// [!!!Error] potential null pointer dereference on '<pointer>'
// ↳ control flow proves pointer is null on this branch before dereference

// at line 32, column 26
// [!!!Error] potential null pointer dereference on '<pointer>'
// ↳ a preceding local-slot store sets the pointer to null before use

// at line 31, column 7
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 'p' is used after a release in this function
// ↳ a later dereference/call argument use may access invalid memory

// at line 32, column 27
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 'p' is used after a release in this function
// ↳ a later dereference/call argument use may access invalid memory
