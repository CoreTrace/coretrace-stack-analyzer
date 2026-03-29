// SPDX-License-Identifier: Apache-2.0
/**
 * 08 - COMMAND INJECTION (CWE-78)
 *
 * Compile: gcc -Wall -Wextra -g 08_command_injection.c -o 08_test
 * Analyze: clang --analyze 08_command_injection.c
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 8a. Injection via system() */
void vuln_command_injection(const char* filename)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "cat %s", filename);
    system(cmd); /* CWE-78: filename peut contenir "; rm -rf /" */
}

/* 8b. Injection via popen() */
void vuln_popen_injection(const char* host)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ping -c 1 %s", host);
    FILE* fp = popen(cmd, "r"); /* CWE-78: host = "8.8.8.8; cat /etc/shadow" */
    if (fp)
    {
        char buf[512];
        while (fgets(buf, sizeof(buf), fp))
            printf("%s", buf);
        pclose(fp);
    }
}

int main(void)
{
    printf("=== 08: Command Injection Tests ===\n");
    /* Exemples inoffensifs pour la démonstration */
    vuln_command_injection("/etc/hostname");
    vuln_popen_injection("127.0.0.1");
    return 0;
}

// run_test expectations
// resource-model: models/resource-lifetime/generic.txt
// escape-model: models/stack-escape/generic.txt
// buffer-model: models/buffer-overflow/generic.txt

// at line 14, column 1
// [ !!Warn ] local variable 'cmd' is never initialized
// ↳ declared without initializer and no definite write was found in this function

// at line 16, column 5
// [ !!Warn ] potential command injection: non-literal command reaches 'system'
// ↳ the command argument is not a compile-time string literal
// ↳ validate/sanitize external input or avoid shell command composition

// at line 23, column 16
// [ !!Warn ] potential command injection: non-literal command reaches 'popen'
// ↳ the command argument is not a compile-time string literal
// ↳ validate/sanitize external input or avoid shell command composition
