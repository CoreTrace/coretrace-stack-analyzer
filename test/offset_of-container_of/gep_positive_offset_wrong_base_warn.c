#include <stddef.h>
#include <stdint.h>

struct A
{
    int32_t a;
    int32_t b;
    int32_t c;
    int32_t i;
};

int test_gep_positive_offset_missed(void)
{
    struct A obj = {0};
    int32_t* pb = &obj.b;

    // Wrong reconstruction with a positive offset (+4).
    // Correct behavior: diagnostic (base points inside the object, not at the base).
    // at line 27, column 48
    // [!!] potential UB: invalid base reconstruction via offsetof/container_of
    //     variable: 'obj'
    //     source member: offset +4
    //     offset applied: +4 bytes
    //     target type: ptr
    //     [WARNING] unable to verify that derived pointer points to a valid object
    //                 (potential undefined behavior if offset is incorrect)
    struct A* bad_base = (struct A*)((char*)pb + 4);

    // Expected: invalid base reconstruction diagnostic.
    // at line 38, column 22
    // [!!] potential UB: invalid base reconstruction via offsetof/container_of
    //     variable: 'obj'
    //     source member: offset +8
    //     offset applied: +0 bytes
    //     target type: ptr
    //     [WARNING] unable to verify that derived pointer points to a valid object
    //                 (potential undefined behavior if offset is incorrect)
    return bad_base->a;
}

int main(void)
{
    return test_gep_positive_offset_missed();
}