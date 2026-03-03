/**
 * 07 - RACE CONDITION / TOCTOU (CWE-367)
 *
 * Compile: gcc -Wall -Wextra -g 07_toctou.c -o 07_test
 * Analyze: clang --analyze 07_toctou.c
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

/* 7a. TOCTOU classique : access() puis fopen() */
void vuln_toctou(const char* filename)
{
    if (access(filename, R_OK) == 0)
    {
        /* CWE-367: le fichier peut être remplacé par un symlink
         * entre access() et fopen() (race window) */
        FILE* f = fopen(filename, "r");
        if (f)
        {
            char buf[256];
            fgets(buf, sizeof(buf), f);
            printf("content: %s\n", buf);
            fclose(f);
        }
    }
}

/* 7b. TOCTOU : stat() puis open() */
void vuln_toctou_stat(const char* path)
{
    struct stat st;
    if (stat(path, &st) == 0)
    {
        if (S_ISREG(st.st_mode))
        {
            /* CWE-367: path peut avoir changé entre stat() et open() */
            int fd = open(path, O_RDONLY);
            if (fd >= 0)
            {
                char buf[128];
                read(fd, buf, sizeof(buf));
                close(fd);
            }
        }
    }
}

/* Correction : utiliser open() + fstat() sur le fd */
void safe_open(const char* path)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return;
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
    {
        char buf[128];
        read(fd, buf, sizeof(buf));
    }
    close(fd);
}

int main(void)
{
    printf("=== 07: TOCTOU Tests ===\n");
    vuln_toctou("/etc/hostname");
    vuln_toctou_stat("/etc/hostname");
    safe_open("/etc/hostname");
    return 0;
}

// run_test expectations
// resource-model: models/resource-lifetime/generic.txt
// escape-model: models/stack-escape/generic.txt
// buffer-model: models/buffer-overflow/generic.txt

// at line 20, column 1
// [ !!Warn ] local variable 'buf' is never initialized
// ↳ declared without initializer and no definite write was found in this function

// at line 18, column 19
// [ !!Warn ] potential TOCTOU race: path checked with 'access' then used with 'fopen'
// ↳ the file target may change between check and use operations
// ↳ prefer descriptor-based validation (open + fstat) on the same handle

// at line 34, column 22
// [ !!Warn ] potential TOCTOU race: path checked with 'stat' then used with 'open'
// ↳ the file target may change between check and use operations
// ↳ prefer descriptor-based validation (open + fstat) on the same handle
