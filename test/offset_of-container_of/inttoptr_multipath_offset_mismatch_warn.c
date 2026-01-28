#include <stddef.h>
#include <stdint.h>

struct A
{
    int32_t a;
    int32_t b;
    int32_t c;
    int32_t i;
};

int test_multipath_select(int cond)
{
    struct A obj = {0};

    // Multi-path source: either &obj.b or &obj.c.
    // Correct behavior: diagnostic because when cond == 0, the base
    // reconstruction uses the wrong member offset.
    int32_t *p = cond ? &obj.b : &obj.c;

    uintptr_t addr = (uintptr_t)p;
    addr -= offsetof(struct A, b); // uses offset of b
    // at line 31, column 22
    // [!!] potential UB: invalid base reconstruction via offsetof/container_of
    //     variable: 'obj'
    //     source member: offsets +4, +8
    //     offset applied: -4 bytes
    //     target type: ptr
    //     [WARNING] unable to verify that derived pointer points to a valid object
    //                 (potential undefined behavior if offset is incorrect)
    struct A *base = (struct A *)addr;

    // Expected: invalid base reconstruction diagnostic.
    return base->a;
}

int main(void)
{
    test_multipath_select(0);
    test_multipath_select(1);
    return 0;
}
