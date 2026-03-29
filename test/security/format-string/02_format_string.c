// SPDX-License-Identifier: Apache-2.0
/**
 * 02 - FORMAT STRING (CWE-134)
 *
 * Compile: gcc -Wall -Wextra -g -fsanitize=address 02_format_string.c -o 02_test
 * Analyze: clang --analyze 02_format_string.c
 */

#include <stdio.h>
#include <string.h>

/* 2a. Format string directe */
void vuln_format_string(const char* user_input)
{
    printf(user_input); /* CWE-134: l'attaquant contrôle le format */
}

/* 2b. Format string via snprintf + fprintf */
void vuln_format_string_log(const char* msg)
{
    char buf[256];
    snprintf(buf, sizeof(buf), msg); /* CWE-134: format contrôlé */
    fprintf(stderr, buf);            /* CWE-134: double vulnérabilité */
}

int main(void)
{
    printf("=== 02: Format String Tests ===\n");
    vuln_format_string("%x %x %x %x\n");
    vuln_format_string_log("Hello %s%s%s%s\n");
    return 0;
}

// run_test expectations
// resource-model: models/resource-lifetime/generic.txt
// escape-model: models/stack-escape/generic.txt
// buffer-model: models/buffer-overflow/generic.txt

// at line 13, column 12
// [ !!Warn ] non-literal format string may allow format injection
// ↳ clang: format string is not a string literal (potentially insecure)

// at line 18, column 1
// [ !!Warn ] local variable 'buf' is never initialized
// ↳ declared without initializer and no definite write was found in this function

// at line 19, column 32
// [ !!Warn ] non-literal format string may allow format injection
// ↳ clang: format string is not a string literal (potentially insecure)

// at line 20, column 21
// [ !!Warn ] non-literal format string may allow format injection
// ↳ clang: format string is not a string literal (potentially insecure)
