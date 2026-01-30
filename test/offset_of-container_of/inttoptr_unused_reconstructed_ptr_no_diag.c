#include <stddef.h>
#include <stdint.h>

struct A
{
    int32_t a;
    int32_t b;
    int32_t c;
    int32_t i;
};

int test_inttoptr_unused_pointer_noise(void)
{
    struct A obj = {0};
    int32_t* pb = &obj.b;

    uintptr_t addr = (uintptr_t)pb;
    addr -= offsetof(struct A, b); // correct offset, should reconstruct obj
    struct A* base = (struct A*)addr;

    // No dereference or use as struct A*.
    // Correct behavior: no diagnostic (avoid noise when unused).
    if ((uintptr_t)base == 0u)
    {
        return 1;
    }
    return 0;
}

int main(void)
{
    return test_inttoptr_unused_pointer_noise();
}
