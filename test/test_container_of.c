#include <stddef.h>
#include <stdint.h>

struct MyStruct {
    int32_t field_a;  // offset 0
    int32_t field_b;  // offset 4
    int32_t field_c;  // offset 8
};

void test_invalid_container_of(void)
{
    struct MyStruct obj;
    int32_t *ptr_b = &obj.field_b;
    
    // BUG: using wrong offset (8 instead of 4)
    struct MyStruct *wrong_base = (struct MyStruct *)((char *)ptr_b - 8);
    
    wrong_base->field_a = 42;  // UB: out of bounds access
}

void test_valid_container_of(void)
{
    struct MyStruct obj;
    int32_t *ptr_b = &obj.field_b;
    
    // CORRECT: using correct offset (4)
    struct MyStruct *correct_base = (struct MyStruct *)((char *)ptr_b - 4);
    
    correct_base->field_a = 42;  // OK
}

int main(void)
{
    test_invalid_container_of();
    test_valid_container_of();
    return 0;
}
