// SPDX-License-Identifier: Apache-2.0
/**
 * 10 - TYPE CONFUSION / MAUVAIS CAST (CWE-843)
 *
 * Compile: gcc -Wall -Wextra -g -fsanitize=undefined 10_type_confusion.c -o 10_test
 * Analyze: clang --analyze 10_type_confusion.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    int type;
    int value;
} BaseObj;
typedef struct
{
    int type;
    int value;
    char extra[64];
} ExtObj;

/* 10a. Cast non vérifié via void* */
void vuln_type_confusion(void* obj)
{
    BaseObj* base = (BaseObj*)obj;
    if (base->type == 1)
    {
        /* CWE-843: on suppose que c'est un ExtObj sans vérification réelle */
        ExtObj* ext = (ExtObj*)obj;
        printf("extra = %s\n", ext->extra); /* lecture hors bornes si BaseObj */
    }
}

/* 10b. Union type punning dangereux */
typedef union
{
    int as_int;
    float as_float;
    char* as_ptr;
} Variant;

void vuln_union_confusion(Variant v, int expected_type)
{
    /* Aucune vérification que expected_type correspond au champ actif */
    if (expected_type == 0)
        printf("int: %d\n", v.as_int);
    else if (expected_type == 1)
        printf("float: %f\n", v.as_float);
    else
        printf("ptr: %s\n", v.as_ptr); /* CWE-843: crash si as_ptr invalide */
}

int main(void)
{
    printf("=== 10: Type Confusion Tests ===\n");

    /* Passe un BaseObj là où un ExtObj est attendu */
    BaseObj b = {1, 42};
    vuln_type_confusion(&b); /* va lire ext->extra hors bornes */

    /* Mauvais tag d'union */
    Variant v;
    v.as_int = 12345;
    vuln_union_confusion(v, 2); /* interprète un int comme un pointeur */

    return 0;
}

// run_test expectations
// resource-model: models/resource-lifetime/generic.txt
// escape-model: models/stack-escape/generic.txt
// buffer-model: models/buffer-overflow/generic.txt

// at line 21, column 9
// [ !!Warn ] potential type confusion: incompatible struct views on the same pointer
// ↳ smaller observed view: 'struct.BaseObj' (8 bytes)
// ↳ accessed view: 'struct.ExtObj' at byte offset 8
// ↳ field access may read/write outside the actual object layout

// at line 52, column 5
// [ !!Warn ] potential read of uninitialized local variable 'v'
// ↳ this load may execute before any definite initialization on all control-flow paths
