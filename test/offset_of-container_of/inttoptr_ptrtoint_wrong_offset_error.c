#include <stdint.h>
#include <stddef.h>

struct Data {
    int x;      // offset 0
    int y;      // offset 4
    int z;      // offset 8
};

void test_ptrtoint_pattern(void)
{
    struct Data obj;
    int *ptr_y = &obj.y;

    // Pattern: inttoptr(ptrtoint(ptr) - offset)
    // BUG: using wrong offset (8 instead of 4)
    uintptr_t addr = (uintptr_t)ptr_y;
    addr -= 8;
    // at line 27, column 31
    // [!!] potential UB: invalid base reconstruction via offsetof/container_of
    //     variable: 'obj'
    //     source member: offset +4
    //     offset applied: -8 bytes
    //     target type: ptr
    //     [ERROR] derived pointer points OUTSIDE the valid object range
    //             (this will cause undefined behavior if dereferenced)
    struct Data *wrong_base = (struct Data *)addr;

    wrong_base->x = 100;  // UB: out of bounds
}

int main(void) {
    test_ptrtoint_pattern();
    return 0;
}
