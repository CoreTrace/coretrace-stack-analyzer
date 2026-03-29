// SPDX-License-Identifier: Apache-2.0
typedef struct IntOutProps
{
    int* out;
} IntOutProps;

extern void fill_wrapper_cross_tu(const IntOutProps* props);

// Expected behavior:
// - with cross-TU uninitialized summaries enabled and
//   cross-tu-uninitialized-wrapper-def.c analyzed in the same run,
//   this file should not emit an uninitialized warning.
// - if analyzed alone, a local-TU warning on 'value' is expected.
int cross_tu_read_after_wrapper(void)
{
    int value;
    IntOutProps props = {&value};
    fill_wrapper_cross_tu(&props);
    return value;
}

// at line 18, column 12
// [ !!Warn ] potential read of uninitialized local variable 'value'
//          ↳ this load may execute before any definite initialization on all control-flow paths
