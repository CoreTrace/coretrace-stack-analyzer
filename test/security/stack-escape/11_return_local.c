// SPDX-License-Identifier: Apache-2.0
/**
 * 11 - RETURN POINTER TO LOCAL (CWE-562)
 *
 * Compile: gcc -Wall -Wextra -g 11_return_local.c -o 11_test
 * Analyze: clang --analyze 11_return_local.c
 */

#include <stdio.h>
#include <string.h>

/* 11a. Retour d'adresse de buffer local */
char* vuln_return_local(void)
{
    char buf[64];
    strcpy(buf, "data on stack");
    return buf; /* CWE-562: buf est détruit au retour de la fonction */
}

/* 11b. Retour d'adresse de variable locale via pointeur */
int* vuln_return_local_int(void)
{
    int x = 42;
    return &x; /* CWE-562: adresse de variable locale */
}

/* 11c. Plus subtil : tableau local dans un struct retourné par pointeur */
typedef struct
{
    char* data;
} Wrapper;

Wrapper vuln_return_local_struct(void)
{
    char tmp[128];
    strcpy(tmp, "temporary data");
    Wrapper w;
    w.data = tmp; /* CWE-562: tmp sera invalide après le retour */
    return w;
}

int main(void)
{
    printf("=== 11: Return Pointer to Local Tests ===\n");

    char* s = vuln_return_local();
    printf("local string: %s\n", s); /* UB: accès à mémoire stack invalide */

    int* p = vuln_return_local_int();
    printf("local int: %d\n", *p); /* UB */

    Wrapper w = vuln_return_local_struct();
    printf("local struct data: %s\n", w.data); /* UB */

    return 0;
}

// run_test expectations
// resource-model: models/resource-lifetime/generic.txt
// escape-model: models/stack-escape/generic.txt
// buffer-model: models/buffer-overflow/generic.txt

// at line 14, column 5
// [ !!Warn ] potential stack buffer overflow in __strcpy_chk on variable 'buf'
// ↳ destination stack buffer size: 64 bytes
// ↳ this API has no explicit size argument; destination fit cannot be proven statically

// at line 13, column 1
// [ !!Warn ] local variable 'buf' is never initialized
// ↳ declared without initializer and no definite write was found in this function

// at line 15, column 5
// [ !!Warn ] stack pointer escape: address of variable 'buf' escapes this function
// ↳ escape via return statement (pointer to stack returned to caller)

// at line 21, column 5
// [ !!Warn ] stack pointer escape: address of variable 'x' escapes this function
// ↳ escape via return statement (pointer to stack returned to caller)

// at line 31, column 5
// [ !!Warn ] potential stack buffer overflow in __strcpy_chk on variable 'tmp'
// ↳ destination stack buffer size: 128 bytes
// ↳ this API has no explicit size argument; destination fit cannot be proven statically

// at line 30, column 1
// [ !!Warn ] local variable 'tmp' is never initialized
// ↳ declared without initializer and no definite write was found in this function
