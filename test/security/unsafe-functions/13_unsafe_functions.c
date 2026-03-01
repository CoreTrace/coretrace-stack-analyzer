/**
 * 13 - UNSAFE / DEPRECATED FUNCTIONS (CWE-676)
 *
 * Compile: gcc -Wall -Wextra -g 13_unsafe_functions.c -o 13_test
 * Analyze: clang --analyze 13_unsafe_functions.c
 *
 * Note: gets() est retiré depuis C11, certains compilateurs refusent de compiler.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 13a. gets() — jamais sûr, retiré en C11 */
void vuln_gets(void)
{
    char buf[64];
    printf("Input: ");
    gets(buf); /* CWE-676: aucune limite de taille, overflow garanti */
    printf("Got: %s\n", buf);
}

/* 13b. strcat() sans vérification de taille */
void vuln_strcat(const char* src)
{
    char buf[16] = "Hello ";
    strcat(buf, src); /* CWE-676: pas de limite → overflow */
    puts(buf);
}

/* 13c. strtok() — non réentrant, non thread-safe */
void vuln_strtok(char* input)
{
    char* tok = strtok(input, " "); /* CWE-676: modifie input, état global */
    while (tok)
    {
        printf("token: %s\n", tok);
        tok = strtok(NULL, " ");
    }
}

/* 13d. atoi() — pas de gestion d'erreur */
void vuln_atoi(const char* input)
{
    int val = atoi(input); /* CWE-676: pas de détection d'erreur/overflow */
    printf("val = %d\n", val);
    /* Utiliser strtol() avec vérification d'errno à la place */
}

/* 13e. strcpy() — classique */
void vuln_strcpy(const char* input)
{
    char buf[8];
    strcpy(buf, input); /* CWE-676: pas de vérification de taille */
    puts(buf);
}

int main(void)
{
    printf("=== 13: Unsafe Functions Tests ===\n");
    vuln_strcat("AAAAAAAAAAAAAAAAAAAAA");
    char data[] = "hello world foo bar";
    vuln_strtok(data);
    vuln_atoi("not_a_number");
    vuln_strcpy("way too long for this tiny buffer");
    return 0;
}

// run_test expectations
// resource-model: models/resource-lifetime/generic.txt
// escape-model: models/stack-escape/generic.txt
// buffer-model: models/buffer-overflow/generic.txt

// at line 16, column 1
// [ !!Warn ] local variable 'buf' is never initialized
// ↳ declared without initializer and no definite write was found in this function

// at line 18, column 5
// [ !!Warn ] deprecated unsafe function 'gets' is used
// ↳ clang: 'gets' is deprecated: This function is provided for compatibility reasons only.  Due to security concerns inherent in the design of gets(3), it is highly recommended that you use fgets(3) instead.

// at line 25, column 5
// [ !!Warn ] potential stack buffer overflow in __strcat_chk on variable 'buf'
// ↳ destination stack buffer size: 16 bytes
// ↳ this API has no explicit size argument; destination fit cannot be proven statically

// at line 48, column 5
// [ !!Warn ] potential stack buffer overflow in __strcpy_chk on variable 'buf'
// ↳ destination stack buffer size: 8 bytes
// ↳ this API has no explicit size argument; destination fit cannot be proven statically

// at line 47, column 1
// [ !!Warn ] local variable 'buf' is never initialized
// ↳ declared without initializer and no definite write was found in this function
