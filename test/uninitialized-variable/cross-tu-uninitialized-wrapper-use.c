typedef struct IntOutProps
{
    int* out;
} IntOutProps;

extern void fill_wrapper_cross_tu(const IntOutProps* props);

int cross_tu_read_after_wrapper(void)
{
    int value;
    IntOutProps props = {&value};
    fill_wrapper_cross_tu(&props);
    return value;
}
