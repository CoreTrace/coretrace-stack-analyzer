#include <cstdint>

int main(void)
{
    uint32_t test0;
    int32_t test1;
    uint16_t test2;
    int16_t test3;
    uint8_t test4;
    int8_t test5;

    return 0;
}

// at line 5, column 1
// [ !!Warn ] local variable 'test0' is never initialized
//          ↳ declared without initializer and no definite write was found in this function

// at line 6, column 1
// [ !!Warn ] local variable 'test1' is never initialized
//          ↳ declared without initializer and no definite write was found in this function

// at line 7, column 1
// [ !!Warn ] local variable 'test2' is never initialized
//          ↳ declared without initializer and no definite write was found in this function

// at line 8, column 1
// [ !!Warn ] local variable 'test3' is never initialized
//          ↳ declared without initializer and no definite write was found in this function

// at line 9, column 1
// [ !!Warn ] local variable 'test4' is never initialized
//          ↳ declared without initializer and no definite write was found in this function

// at line 10, column 1
// [ !!Warn ] local variable 'test5' is never initialized
//          ↳ declared without initializer and no definite write was found in this function
