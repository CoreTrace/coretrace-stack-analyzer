// Multi-TU test: File 1 - Helper functions with various stack usages

#include "complex-cross-tu-common.h"

void helper_process_small(void) {
    char buf[512];
    buf[0] = 0;
}

void helper_process_large(void) {
    char buf[500000];
    buf[0] = 0;
}

void helper_accumulate(int count) {
    char buf[256];
    for (int i = 0; i < count; i++) {
        helper_process_small();
    }
    buf[0] = 0;
}
