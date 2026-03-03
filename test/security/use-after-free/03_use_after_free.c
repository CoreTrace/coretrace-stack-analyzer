/**
 * 03 - USE-AFTER-FREE / DOUBLE FREE (CWE-415, CWE-416)
 *
 * Compile: gcc -Wall -Wextra -g -fsanitize=address 03_use_after_free.c -o 03_test
 * Analyze: clang --analyze 03_use_after_free.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 3a. Use-after-free simple */
void vuln_use_after_free(void)
{
    char* ptr = (char*)malloc(64);
    if (!ptr)
        return;
    strcpy(ptr, "hello");
    free(ptr);
    printf("data = %s\n", ptr); /* CWE-416: accès après free */
}

/* 3b. Double free conditionnel */
void vuln_double_free(int condition)
{
    char* p = (char*)malloc(128);
    if (!p)
        return;
    if (condition)
    {
        free(p);
    }
    /* ... du code ... */
    free(p); /* CWE-415: double free si condition == true */
}

/* 3c. Dangling pointer dans une struct */
typedef struct
{
    char* name;
    int id;
} User;

User* vuln_dangling_struct(void)
{
    User* u = (User*)malloc(sizeof(User));
    if (!u)
        return NULL;
    u->name = (char*)malloc(32);
    if (!u->name)
    {
        free(u);
        return NULL;
    }
    strcpy(u->name, "Alice");
    free(u->name);
    /* u->name est maintenant dangling, mais u est retourné */
    return u; /* CWE-416: l'appelant accédera à u->name */
}

int main(void)
{
    printf("=== 03: Use-After-Free / Double Free Tests ===\n");
    vuln_use_after_free();
    vuln_double_free(1);
    User* u = vuln_dangling_struct();
    if (u)
    {
        printf("name = %s\n", u->name); /* dangling */
        free(u);
    }
    return 0;
}

// run_test expectations
// resource-model: models/resource-lifetime/generic.txt
// escape-model: models/stack-escape/generic.txt
// buffer-model: models/buffer-overflow/generic.txt

// at line 18, column 27
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 'ptr' is used after a release in this function
// ↳ a later dereference/call argument use may access invalid memory

// at line 29, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'p' is released without a matching acquire in this function
// ↳ this may indicate release-after-release or ownership mismatch

// at line 44, column 5
// [ !!Warn ] released handle derived from 'u' may escape through a returned owner object
// ↳ caller-visible object may contain dangling pointer state
