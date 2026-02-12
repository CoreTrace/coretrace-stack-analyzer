#include <stdint.h>
#include <stddef.h>

struct Data
{
    int x; // offset 0
    int y; // offset 4
    int z; // offset 8
};

void test_ptrtoint_pattern(void)
{
    struct Data obj;
    int* ptr_y = &obj.y;

    // Pattern: inttoptr(ptrtoint(ptr) - offset)
    // BUG: using wrong offset (8 instead of 4)
    uintptr_t addr = (uintptr_t)ptr_y;
    addr -= 8;

    struct Data* wrong_base = (struct Data*)addr;

    wrong_base->x = 100; // UB: out of bounds
}

int main(void)
{
    test_ptrtoint_pattern();
    return 0;
}

// at line 13, column 1
// [ !!Warn ] local variable 'obj' is never initialized
//          ↳ declared without initializer and no definite write was found in this function

// at line 21, column 31
// [ !!Warn ] potential UB: invalid base reconstruction via offsetof/container_of
//          ↳ variable: 'obj'
//          ↳ source member: offset +4
//          ↳ offset applied: -8 bytes
//          ↳ target type: ptr
// [!!!Error] derived pointer points OUTSIDE the valid object range
//          ↳ (this will cause undefined behavior if dereferenced)

// at line 23, column 17
// [ !!Warn ] potential UB: invalid base reconstruction via offsetof/container_of
//          ↳ variable: 'obj'
//          ↳ source member: offsets +-4, +4
//          ↳ offset applied: +0 bytes
//          ↳ target type: ptr
// [!!!Error] derived pointer points OUTSIDE the valid object range
//          ↳ (this will cause undefined behavior if dereferenced)
