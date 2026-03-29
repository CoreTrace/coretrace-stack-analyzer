// SPDX-License-Identifier: Apache-2.0
/**
 * 14 - VARIADIC FUNCTION MISUSE (format/argument mismatch)
 *
 * Compile: gcc -Wall -Wextra -g 14_variadic_mismatch.c -o 14_test
 * Analyze: clang --analyze 14_variadic_mismatch.c
 */

#include <stdio.h>

/* 14a. Mismatch type : %s attend char*, reçoit int */
void vuln_format_type_mismatch(void)
{
    int x = 42;
    printf("%s\n", x); /* UB: %s attend un pointeur, reçoit un int */
}

/* 14b. Pas assez d'arguments */
void vuln_format_missing_args(void)
{
    int x = 42;
    printf("%d %d %d\n", x); /* UB: 2 arguments manquants */
}

/* 14c. Trop d'arguments (pas UB mais suspect) */
void vuln_format_extra_args(void)
{
    printf("%d\n", 1, 2, 3); /* args 2 et 3 ignorés, probablement un bug */
}

/* 14d. Mismatch signed/unsigned */
void vuln_format_signedness(void)
{
    unsigned int u = 4294967295U;
    printf("signed: %d\n", u); /* affiche -1, pas la valeur attendue */

    int neg = -1;
    printf("unsigned: %u\n", neg); /* affiche 4294967295, trompeur */
}

/* 14e. Mismatch taille : %d pour un long long */
void vuln_format_size_mismatch(void)
{
    long long big = 1LL << 40;
    printf("value: %d\n", big); /* UB: %d attend int, reçoit long long */
}

int main(void)
{
    printf("=== 14: Variadic Mismatch Tests ===\n");
    vuln_format_type_mismatch();
    vuln_format_missing_args();
    vuln_format_extra_args();
    vuln_format_signedness();
    vuln_format_size_mismatch();
    return 0;
}

// run_test expectations
// resource-model: models/resource-lifetime/generic.txt
// escape-model: models/stack-escape/generic.txt
// buffer-model: models/buffer-overflow/generic.txt

// at line 13, column 20
// [ !!Warn ] variadic format and argument list appear inconsistent
// ↳ clang: format specifies type 'char *' but the argument has type 'int'

// at line 19, column 17
// [ !!Warn ] variadic format and argument list appear inconsistent
// ↳ clang: more '%' conversions than data arguments

// at line 24, column 23
// [ !!Warn ] variadic format and argument list appear inconsistent
// ↳ clang: data argument not used by format string

// at line 39, column 27
// [ !!Warn ] variadic format and argument list appear inconsistent
// ↳ clang: format specifies type 'int' but the argument has type 'long long'
