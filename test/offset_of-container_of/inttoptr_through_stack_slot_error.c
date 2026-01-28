#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct A {
    int32_t a;
    int32_t b;
    int32_t c;
    int32_t i;
};

int test_inttoptr_load_store(void)
{
    struct A obj = {0};

    int32_t *tmp = &obj.b;
    int32_t *p = tmp; // forces load/store chain at -O0

    uintptr_t addr = (uintptr_t)p;
    addr -= offsetof(struct A, i); // wrong offset (12 instead of 4)
    // at line 29, column 26
    // [!!] potential UB: invalid base reconstruction via offsetof/container_of
    //     variable: 'obj'
    //     source member: offset +4
    //     offset applied: -12 bytes
    //     target type: ptr
    //     [ERROR] derived pointer points OUTSIDE the valid object range
    //             (this will cause undefined behavior if dereferenced)
    struct A *bad_base = (struct A *)addr;

    return bad_base->a;
}
