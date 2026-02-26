#include <stddef.h>

// Test cases at stack limit boundaries
// Default stack limit: 8388608 bytes (8 MiB)

// at line 8, column 0
// [!Warning] function 'at_limit_lower' creates large local frame
//          ↳ local stack: 4194304 bytes
//          ↳ half of default stack limit (8388608 bytes)
void at_limit_lower(void) {
    char buf[4194304];
    buf[0] = 0;
}

// at line 16, column 0
// [!Warning] function 'at_limit_quarter' creates medium local frame
//          ↳ local stack: 2097152 bytes
//          ↳ quarter of default stack limit
void at_limit_quarter(void) {
    char buf[2097152];
    buf[0] = 0;
}

// at line 24, column 0
// [!Warning] function 'just_below_limit' approaches stack limit
//          ↳ local stack: 8000000 bytes
//          ↳ approaching stack limit (8388608 bytes)
void just_below_limit(void) {
    char buf[8000000];
    buf[0] = 0;
}

// Safe function - well below limit
void safe_allocation(void) {
    char buf[1024];
    buf[0] = 0;
}
