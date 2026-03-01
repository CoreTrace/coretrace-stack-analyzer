/**
 * 12 - OUT-OF-BOUNDS READ (CWE-125)
 *
 * Compile: gcc -Wall -Wextra -g -fsanitize=address 12_oob_read.c -o 12_test
 * Analyze: clang --analyze 12_oob_read.c
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 12a. Lecture hors limites d'un tableau (pas de bounds check) */
int vuln_oob_read(int* arr, int size, int index)
{
    (void)size;        /* ignoré volontairement */
    return arr[index]; /* CWE-125: index peut être >= size ou négatif */
}

/* 12b. strlen sur un buffer non terminé par \0 */
size_t vuln_missing_null_term(void)
{
    char buf[8];
    memcpy(buf, "AAAAAAAA", 8); /* pas de '\0' terminal */
    return strlen(buf);         /* CWE-125: lit au-delà du buffer */
}

/* 12c. Lecture heap hors bornes via index non validé */
void vuln_heap_oob_read(int user_index)
{
    int* table = (int*)malloc(10 * sizeof(int));
    if (!table)
        return;
    for (int i = 0; i < 10; i++)
        table[i] = i * 10;

    /* CWE-125: user_index pas vérifié */
    printf("value = %d\n", table[user_index]);
    free(table);
}

/* 12d. Lecture après la fin d'une chaîne courte dans un buffer fixe */
void vuln_short_string_read(void)
{
    char buf[64];
    memset(buf, 0, sizeof(buf));
    strcpy(buf, "Hi");

    /* Quelqu'un suppose que buf contient au moins 10 caractères */
    for (int i = 0; i < 10; i++)
    {
        printf("%02x ", (unsigned char)buf[i]); /* lit des zéros, pas un crash */
    }
    /* Mais dans un vrai scénario, buf pourrait ne pas être zéro-initialisé → info leak */
    printf("\n");
}

int main(void)
{
    printf("=== 12: Out-of-Bounds Read Tests ===\n");

    int arr[] = {10, 20, 30};
    printf("oob: %d\n", vuln_oob_read(arr, 3, 10));

    printf("strlen no null: %zu\n", vuln_missing_null_term());

    vuln_heap_oob_read(50); /* index 50, tableau de taille 10 */

    vuln_short_string_read();

    return 0;
}

// run_test expectations
// resource-model: models/resource-lifetime/generic.txt
// escape-model: models/stack-escape/generic.txt
// buffer-model: models/buffer-overflow/generic.txt

// at line 22, column 12
// [ !!Warn ] potential out-of-bounds read: string buffer 'buf' may be missing a null terminator before 'strlen'
// ↳ buffer size: 8 bytes, last write size: 8 bytes
// ↳ unterminated strings can make read APIs scan past buffer bounds

// at line 32, column 28
// [ !!Warn ] potential out-of-bounds read on heap buffer 'call' via unchecked index
// ↳ inferred heap capacity: 10 element(s)
// ↳ index value is not proven to be within [0, capacity-1]

// at line 40, column 5
// [ !!Warn ] potential stack buffer overflow in __strcpy_chk on variable 'buf'
// ↳ destination stack buffer size: 64 bytes
// ↳ this API has no explicit size argument; destination fit cannot be proven statically
