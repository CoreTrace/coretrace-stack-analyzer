// Multi-TU test: Header file with shared declarations

#ifndef COMPLEX_CROSS_TU_COMMON_H
#define COMPLEX_CROSS_TU_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif

// Helper functions in complex-cross-tu-1.c
void helper_process_small(void);
void helper_process_large(void);
void helper_accumulate(int count);

// Orchestrator functions in complex-cross-tu-2.c
void orchestrate_simple(void);
void orchestrate_complex(int iterations);

// Entry point in complex-cross-tu-main.c
int main_multi_tu(void);

#ifdef __cplusplus
}
#endif

#endif // COMPLEX_CROSS_TU_COMMON_H
