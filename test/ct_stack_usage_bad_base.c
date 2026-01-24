#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct A {
    int32_t a;
    int32_t b;
    int32_t c;
    int32_t i;
};

int main(void)
{
    struct A obj = { .a = 11, .b = 22, .c = 33, .i = 44 };

    int32_t *pb = &obj.b;

    /* Bug: subtract offset of the WRONG member (i instead of b). */
    struct A *bad_base = (struct A *)((char *)pb - offsetof(struct A, i));

    /* UB: bad_base is not guaranteed to point to a valid struct A object. */
    printf("%d\n", bad_base->a);
    printf("%d\n", bad_base->i);
    return 0;
}
