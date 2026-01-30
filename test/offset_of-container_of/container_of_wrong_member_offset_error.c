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

    /* Bug: subtract offset of the WRONG member (i instead of b). */
    // at line 28, column 48
    // [!!] potential UB: invalid base reconstruction via offsetof/container_of
    //     variable: 'obj'
    //     source member: offset +4
    //     offset applied: -12 bytes
    //     target type: ptr
    //     [ERROR] derived pointer points OUTSIDE the valid object range
    //             (this will cause undefined behavior if dereferenced)
    struct A* bad_base = (struct A*)((char*)pb - offsetof(struct A, i));

    /* UB: bad_base is not guaranteed to point to a valid struct A object. */
    // at line 39, column 30
    // [!!] potential UB: invalid base reconstruction via offsetof/container_of
    //     variable: 'obj'
    //     source member: offset +-8
    //     offset applied: +0 bytes
    //     target type: ptr
    //     [ERROR] derived pointer points OUTSIDE the valid object range
    //             (this will cause undefined behavior if dereferenced)
    printf("%d\n", bad_base->a);
    // at line 48, column 30
    // [!!] potential UB: invalid base reconstruction via offsetof/container_of
    //     variable: 'obj'
    //     source member: offset +-8
    //     offset applied: +12 bytes
    //     target type: ptr
    //     [WARNING] unable to verify that derived pointer points to a valid object
    //                 (potential undefined behavior if offset is incorrect)
    printf("%d\n", bad_base->i);
    return 0;
}
