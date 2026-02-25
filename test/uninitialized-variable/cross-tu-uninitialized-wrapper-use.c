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
