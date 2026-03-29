// SPDX-License-Identifier: Apache-2.0
/**
 * 06 - UNINITIALIZED MEMORY (CWE-457, CWE-908, CWE-200)
 *
 * Compile: gcc -Wall -Wextra -g -fsanitize=memory 06_uninitialized.c -o 06_test
 *          (MemorySanitizer requiert clang: clang -fsanitize=memory)
 * Analyze: clang --analyze 06_uninitialized.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 6a. Variable locale non initialisée */
int vuln_uninit_var(int condition)
{
    int x;
    if (condition)
        x = 10;
    return x; /* CWE-457: x non initialisé si condition == false */
}

/* 6b. Lecture de mémoire heap non initialisée */
void vuln_uninit_heap(void)
{
    char* buf = (char*)malloc(64);
    if (!buf)
        return;
    /* pas de memset / initialisation */
    if (buf[0] == 'A')
    { /* CWE-908: lecture non initialisée */
        puts("Found A");
    }
    free(buf);
}

/* 6c. Struct partiellement initialisée → information leak */
typedef struct
{
    int type;
    char name[32];
    int padding;
} Packet;

void vuln_info_leak(int fd)
{
    Packet pkt;
    pkt.type = 1;
    strcpy(pkt.name, "test");
    /* pkt.padding jamais initialisé -> fuite de données stack */
    write(fd, &pkt, sizeof(pkt)); /* CWE-200: info leak */
}

int vuln_info_leak2(int fd)
{
    Packet pkt;
    pkt.type = 1;
    strcpy(pkt.name, "test");
    /* pkt.padding jamais initialisé -> fuite de données stack */
    write(fd, &pkt, sizeof(pkt)); /* CWE-200: info leak */

    return pkt.padding; /* CWE-457: read of uninitialized variable */
}

int main(void)
{
    printf("=== 06: Uninitialized Memory Tests ===\n");
    printf("uninit var: %d\n", vuln_uninit_var(0));
    vuln_uninit_heap();
    vuln_info_leak(STDOUT_FILENO);
    vuln_info_leak2(STDOUT_FILENO);
    return 0;
}

// run_test expectations
// resource-model: models/resource-lifetime/generic.txt
// escape-model: models/stack-escape/generic.txt
// buffer-model: models/buffer-overflow/generic.txt

// at line 19, column 12
// [ !!Warn ] potential read of uninitialized local variable 'x'
// ↳ this load may execute before any definite initialization on all control-flow paths

// at line 43, column 5
// [ !!Warn ] potential stack buffer overflow in __strcpy_chk on variable 'pkt'
// ↳ destination stack buffer size: 40 bytes
// ↳ this API has no explicit size argument; destination fit cannot be proven statically

// at line 45, column 5
// [ !!Warn ] potential information leak: local variable 'pkt' may expose uninitialized bytes through external sink 'write'
// ↳ transmitted range is not fully initialized on all control-flow paths

// at line 51, column 5
// [ !!Warn ] potential stack buffer overflow in __strcpy_chk on variable 'pkt'
// ↳ destination stack buffer size: 40 bytes
// ↳ this API has no explicit size argument; destination fit cannot be proven statically

// at line 53, column 5
// [ !!Warn ] potential information leak: local variable 'pkt' may expose uninitialized bytes through external sink 'write'
// ↳ transmitted range is not fully initialized on all control-flow paths

// at line 55, column 16
// [ !!Warn ] potential read of uninitialized local variable 'pkt'
// ↳ this load may execute before any definite initialization on all control-flow paths
