// SPDX-License-Identifier: Apache-2.0
#include <stdbool.h>

int main(int argc, char** argv)
{
    int value_int;
    bool value_bool;
    void* value_ptr;

    return 0;
}

// at line 5, column 1
// [ !!Warn ] local variable 'value_int' is never initialized
//          ↳ declared without initializer and no definite write was found in this function

// at line 6, column 1
// [ !!Warn ] local variable 'value_bool' is never initialized
//          ↳ declared without initializer and no definite write was found in this function

// at line 7, column 1
// [ !!Warn ] local variable 'value_ptr' is never initialized
//          ↳ declared without initializer and no definite write was found in this function