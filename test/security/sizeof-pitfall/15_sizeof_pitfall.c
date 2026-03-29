// SPDX-License-Identifier: Apache-2.0
/**
 * 15 - SIZEOF PITFALLS (CWE-467)
 *
 * Compile: gcc -Wall -Wextra -g 15_sizeof_pitfall.c -o 15_test
 * Analyze: clang --analyze 15_sizeof_pitfall.c
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 15a. sizeof sur pointeur au lieu du tableau */
void process(char* buf)
{
    /* sizeof(buf) == sizeof(char*) == 8 sur 64-bit, PAS 256 */
    memset(buf, 0, sizeof(buf)); /* CWE-467: efface seulement 8 octets */
}

void vuln_sizeof_pointer(void)
{
    char buffer[256];
    memset(buffer, 'A', sizeof(buffer));
    process(buffer);
    /* On s'attend à un buffer vide, mais seuls 8 octets sont nuls */
    printf("buf[100] = 0x%02x (devrait être 0x00)\n", (unsigned char)buffer[100]);
}

/* 15b. sizeof sur un tableau passé en paramètre */
void vuln_array_param_sizeof(int arr[100])
{
    /* arr est en fait un int*, sizeof(arr) == sizeof(int*) */
    size_t n = sizeof(arr) / sizeof(arr[0]); /* CWE-467: donne 2 (pas 100) */
    printf("calculated n = %zu (expected 100)\n", n);
}

/* 15c. sizeof d'un pointeur pour une allocation */
void vuln_sizeof_alloc(void)
{
    int* matrix;
    /* Erreur classique : sizeof(matrix) au lieu de sizeof(*matrix) */
    matrix = (int*)malloc(10 * sizeof(matrix)); /* alloue 10 * 8 = 80 */
    /* mais on voulait 10 * sizeof(int) = 40 (gaspillage, ou sous-alloc si inversé) */
    if (matrix)
    {
        matrix[0] = 1;
        free(matrix);
    }
}

/* 15d. sizeof sur un littéral chaîne vs pointeur */
void vuln_sizeof_string(void)
{
    const char* str = "Hello";
    char arr[] = "Hello";

    printf("sizeof(str) = %zu (pointeur: %zu attendu)\n", sizeof(str), sizeof(char*));
    printf("sizeof(arr) = %zu (tableau: 6 attendu, inclut \\0)\n", sizeof(arr));

    /* Bug typique : utiliser sizeof(str) pour copier */
    char dest[32];
    memcpy(dest, str, sizeof(str)); /* copie 8 octets (pointeur), pas 6 */
    dest[sizeof(str)] = '\0';
    printf("dest = '%s' (peut être tronqué ou contenir du garbage)\n", dest);
}

int main(void)
{
    printf("=== 15: sizeof Pitfall Tests ===\n");
    vuln_sizeof_pointer();

    int big_array[100];
    vuln_array_param_sizeof(big_array);

    vuln_sizeof_alloc();
    vuln_sizeof_string();
    return 0;
}

// run_test expectations
// resource-model: models/resource-lifetime/generic.txt
// escape-model: models/stack-escape/generic.txt
// buffer-model: models/buffer-overflow/generic.txt

// at line 15, column 27
// [ !!Warn ] size computation appears to use pointer size instead of object size
// ↳ clang: 'memset' call operates on objects of type 'char' while the size is based on a different type 'char *'

// at line 29, column 22
// [ !!Warn ] size computation appears to use pointer size instead of object size
// ↳ clang: sizeof on array function parameter will return size of 'int *' instead of 'int[100]'

// at line 29, column 28
// [ !!Warn ] size computation appears to use pointer size instead of object size
// ↳ clang: 'sizeof (arr)' will return the size of the pointer, not the array itself

// at line 55, column 30
// [ !!Warn ] size computation appears to use pointer size instead of object size
// ↳ clang: 'memcpy' call operates on objects of type 'const char' while the size is based on a different type 'const char *'

// at line 64, column 1
// [ !!Warn ] local variable 'big_array' is never initialized
// ↳ declared without initializer and no definite write was found in this function
