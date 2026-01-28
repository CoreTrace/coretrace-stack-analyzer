#include <stddef.h>
#include <stdint.h>

struct A {
    int32_t a;
    int32_t b;
    int32_t c;
    int32_t i;
};

int test_ptrtoint_rhs_sub(void)
{
    struct A obj = {0};
    int32_t *pb = &obj.b;

    // ptrtoint is on the RHS: C - ptrtoint(P).
    // Correct behavior: ignore this pattern (not a container_of/offsetof reconstruction).
    uintptr_t addr = 64u - (uintptr_t)pb;
    struct A *base = (struct A *)addr;

    // Expected: no invalid base reconstruction diagnostic.
    return base->a;
}

int main(void)
{
    return test_ptrtoint_rhs_sub();
}
