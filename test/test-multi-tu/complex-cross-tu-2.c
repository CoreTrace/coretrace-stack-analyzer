// Multi-TU test: File 2 - Orchestrator functions coordinating across files

#include "complex-cross-tu-common.h"

// External declarations (from complex-cross-tu-1.c)
extern void helper_process_small(void);
extern void helper_process_large(void);
extern void helper_accumulate(int count);

void orchestrate_simple(void) {
    char buf[256];
    helper_process_small();
    buf[0] = 0;
}

void orchestrate_complex(int iterations) {
    char buf[1024];
    
    helper_process_large();
    
    for (int i = 0; i < iterations; i++) {
        helper_accumulate(i);
    }
    
    buf[0] = 0;
}

int main_multi_tu(void) {
    orchestrate_simple();
    orchestrate_complex(5);
    return 0;
}
