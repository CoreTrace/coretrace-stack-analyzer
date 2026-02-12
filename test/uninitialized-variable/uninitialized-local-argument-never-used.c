#include <stdbool.h>

int main(int argc, char** argv)
{
    int value_int;
    bool value_bool;
    void* value_ptr;

    return 0;
}

// at line 5, column 1
// [!] local variable 'value_int' is never initialized
//     declared without initializer and no definite write was found in this function
