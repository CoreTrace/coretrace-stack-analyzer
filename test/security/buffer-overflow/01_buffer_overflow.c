/**
 * 01 - BUFFER OVERFLOWS (CWE-120, CWE-121, CWE-122, CWE-193)
 *
 * Compile: gcc -Wall -Wextra -g -fsanitize=address 01_buffer_overflow.c -o 01_test
 * Analyze: clang --analyze 01_buffer_overflow.c
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 1a. Stack buffer overflow classique */
void vuln_stack_bof(const char* input)
{
    char buf[16];
    strcpy(buf, input); /* CWE-120: pas de vérification de taille */
    printf("buf = %s\n", buf);
}

/* 1b. Heap buffer overflow (off-by-one) */
void vuln_heap_bof(size_t n)
{
    int* arr = (int*)malloc(n * sizeof(int));
    if (!arr)
        return;
    for (size_t i = 0; i <= n; i++)
    { /* CWE-122: off-by-one, i <= n */
        arr[i] = (int)i;
    }
    free(arr);
}

/* 1c. Buffer overflow via sprintf */
void vuln_sprintf_bof(int user_id, const char* username)
{
    char log_entry[64];
    sprintf(log_entry, "User %d: %s logged in at ...", user_id, username);
    /* CWE-120: sprintf ne vérifie pas la taille */
    puts(log_entry);
}

/* 1d. Off-by-one dans une boucle */
void vuln_off_by_one(void)
{
    char buf[10];
    for (int i = 0; i <= 10; i++)
    { /* CWE-193: écrit buf[10] hors limites */
        buf[i] = 'A';
    }
    buf[9] = '\0';
    puts(buf);
}

int main(void)
{
    printf("=== 01: Buffer Overflow Tests ===\n");
    vuln_stack_bof("AAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    vuln_heap_bof(8);
    vuln_sprintf_bof(1, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    vuln_off_by_one();
    return 0;
}

// run_test expectations
// resource-model: models/resource-lifetime/generic.txt
// escape-model: models/stack-escape/generic.txt
// buffer-model: models/buffer-overflow/generic.txt

// at line 15, column 5
// [ !!Warn ] potential stack buffer overflow in __strcpy_chk on variable 'buf'
// ↳ destination stack buffer size: 16 bytes
// ↳ this API has no explicit size argument; destination fit cannot be proven statically

// at line 14, column 1
// [ !!Warn ] local variable 'buf' is never initialized
// ↳ declared without initializer and no definite write was found in this function

// at line 21, column 23
// [ !!Warn ] potential integer overflow in size computation before 'malloc'
// ↳ operation: mul
// ↳ overflowed size may under-allocate memory or make bounds checks unsound

// at line 31, column 1
// [ !!Warn ] local variable 'log_entry' is never initialized
// ↳ declared without initializer and no definite write was found in this function

// at line 41, column 16
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 10)
// ↳ alias path: buf
// ↳ index variable may go up to 10 (array last valid index: 9)
// ↳ (this is a write access)
