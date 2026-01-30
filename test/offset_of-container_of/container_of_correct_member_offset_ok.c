#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct A
{
    int32_t a;
    int32_t b;
    int32_t c;
    int32_t i;
};

int main(void)
{
    struct A obj = {.a = 11, .b = 22, .c = 33, .i = 44};

    int32_t* pb = &obj.b;

    /* CORRECT: subtract offset of the CORRECT member (b). */
    struct A* good_base = (struct A*)((char*)pb - offsetof(struct A, b));

    /* This should work correctly */
    printf("%d\n", good_base->a);
    printf("%d\n", good_base->i);
    return 0;
}
