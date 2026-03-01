/**
 * 09 - MEMORY LEAKS (CWE-401)
 *
 * Compile: gcc -Wall -Wextra -g -fsanitize=leak 09_memory_leak.c -o 09_test
 * Analyze: clang --analyze 09_memory_leak.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 9a. Fuite sur chemin d'erreur (early return sans free) */
int vuln_leak_error_path(const char* data)
{
    char* buf = (char*)malloc(256);
    if (!buf)
        return -1;

    if (strlen(data) > 255)
    {
        return -1; /* CWE-401: buf jamais libéré sur ce chemin */
    }
    strcpy(buf, data);
    printf("%s\n", buf);
    free(buf);
    return 0;
}

/* 9b. Fuite par écrasement de pointeur */
void vuln_leak_overwrite(void)
{
    char* p = (char*)malloc(100);
    if (!p)
        return;
    p = (char*)malloc(200); /* CWE-401: le premier bloc de 100 est perdu */
    if (p)
        free(p);
}

/* 9c. Fuite dans une boucle */
void vuln_leak_loop(int n)
{
    for (int i = 0; i < n; i++)
    {
        char* tmp = (char*)malloc(64);
        if (!tmp)
            return;
        snprintf(tmp, 64, "item_%d", i);
        printf("%s\n", tmp);
        /* CWE-401: free(tmp) manquant → fuite à chaque itération */
    }
}

/* 9d. Fuite via realloc qui échoue */
void vuln_leak_realloc(void)
{
    char* buf = (char*)malloc(16);
    if (!buf)
        return;
    strcpy(buf, "hello");

    /* Si realloc échoue, il retourne NULL mais ne libère pas buf */
    buf = (char*)realloc(buf, (size_t)-1); /* taille absurde → échec */
    /* CWE-401: si realloc échoue, l'ancien buf est perdu */
    free(buf); /* free(NULL) est safe, mais l'ancien bloc fuit */
}

int main(void)
{
    printf("=== 09: Memory Leak Tests ===\n");
    vuln_leak_error_path("short");
    vuln_leak_error_path("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                         "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                         "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                         "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                         "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                         "AAAAAAAAAA");
    vuln_leak_overwrite();
    vuln_leak_loop(5);
    vuln_leak_realloc();
    return 0;
}

// run_test expectations
// resource-model: models/resource-lifetime/generic.txt
// escape-model: models/stack-escape/generic.txt
// buffer-model: models/buffer-overflow/generic.txt

// at line 28, column 23
// [ !!Warn ] potential resource leak: 'HeapAlloc' acquired in handle 'p' is not released in this function
// ↳ no matching release call was found for the tracked handle

// at line 37, column 29
// [ !!Warn ] potential resource leak: 'HeapAlloc' acquired in handle 'tmp' is not released in this function
// ↳ no matching release call was found for the tracked handle
