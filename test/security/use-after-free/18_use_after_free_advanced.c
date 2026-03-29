// SPDX-License-Identifier: Apache-2.0
/**
 * 18 - ADVANCED USE-AFTER-FREE / DOUBLE FREE CASES (nested if/loop/tricky)
 *
 * Compile: gcc -Wall -Wextra -g -fsanitize=address 18_use_after_free_advanced.c -o 18_test
 * Analyze: clang --analyze 18_use_after_free_advanced.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    char* name;
    int id;
} User18;

/* 18a. UAF dans un nested-if */
void vuln_uaf_nested_if(int gate1, int gate2)
{
    char* p = (char*)malloc(32);
    if (!p)
        return;
    strcpy(p, "nested-uaf");

    if (gate1)
    {
        if (gate2)
        {
            free(p);
        }
    }

    if (gate1 && gate2)
    {
        printf("%s\n", p);
    }

    if (!(gate1 && gate2))
    {
        free(p);
    }
}

/* 18b. Double free dans un nested-loop */
void vuln_double_free_nested_loop(int n)
{
    char* p = (char*)malloc(16);
    if (!p)
        return;

    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j < n; ++j)
        {
            if (i == 0 && j == 0)
            {
                free(p);
            }
        }
    }

    free(p);
}

/* 18c. Dangling field via nested-if puis retour de l'objet owner */
User18* vuln_dangling_nested_if(int gate1, int gate2)
{
    User18* u = (User18*)malloc(sizeof(User18));
    if (!u)
        return NULL;

    u->name = (char*)malloc(32);
    if (!u->name)
    {
        free(u);
        return NULL;
    }

    strcpy(u->name, "alice");
    u->id = 7;

    if (gate1)
    {
        if (gate2)
        {
            free(u->name);
        }
    }

    return u;
}

int main(void)
{
    vuln_uaf_nested_if(1, 1);
    vuln_double_free_nested_loop(1);

    User18* u = vuln_dangling_nested_if(1, 1);
    if (u)
    {
        free(u);
    }

    return 0;
}

// run_test expectations
// resource-model: models/resource-lifetime/generic.txt
// escape-model: models/stack-escape/generic.txt
// buffer-model: models/buffer-overflow/generic.txt

// at line 30, column 24
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 'p' is used after a release in this function
// ↳ a later dereference/call argument use may access invalid memory

// at line 34, column 9
// [!!!Error] potential double release: 'HeapAlloc' handle 'p' is released without a matching acquire in this function
// ↳ this may indicate release-after-release or ownership mismatch

// at line 51, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'p' is released without a matching acquire in this function
// ↳ this may indicate release-after-release or ownership mismatch

// at line 70, column 13
// [ !!Warn ] released handle derived from 'u' may escape through a returned owner object
// ↳ caller-visible object may contain dangling pointer state
