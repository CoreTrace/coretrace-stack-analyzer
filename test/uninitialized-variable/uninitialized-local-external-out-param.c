typedef struct Pair
{
    int a;
    int b;
} Pair;

extern void fill_pair(Pair* out);

int read_after_external_out_param(void)
{
    Pair value;
    fill_pair(&value);
    return value.a + value.b;
}

// not contains: potential read of uninitialized local variable 'value'
