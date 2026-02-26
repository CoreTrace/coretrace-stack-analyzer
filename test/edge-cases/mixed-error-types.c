#include <alloca.h>
#include <stdlib.h>
#include <string.h>

// Single functions demonstrating multiple error types together

// at line 9, column 0
// [!!!Error] function 'multi_error_function' has multiple issues
//          ↳ Issue 1: local frame 4000000 bytes (47% of stack limit)
//          ↳ Issue 2: dynamic alloca with user-controlled size
//          ↳ Issue 3: potential buffer overflow via memcpy
int* multi_error_function(size_t user_size) {
    char local_buf[4000000];  // Large local frame - warning territory
    
    // at line 17, column 18
    // [!!!Error] alloca with user-controlled size
    //          ↳ allocation size 'user_size' comes from user input
    //          ↳ allocation size: unbounded
    char* dyn_buf = (char*)alloca(user_size);
    
    // at line 24, column 4
    // [!!!Error] writing beyond allocated buffer
    //          ↳ memcpy: 100 bytes into 64-byte buffer
    memcpy(dyn_buf, "A", 100);
    
    // at line 29, column 11
    // [!!!Error] returning address of stack-allocated memory
    //          ↳ buffer 'local_buf' will be freed when function returns
    return (int*)local_buf;  // Return stack address - ERROR
}

// at line 34, column 0
// [!Warning] function 'stacked_warnings' combines multiple warning-level issues
void stacked_warnings(int* user_ptr) {
    // at line 38, column 4
    // [!Warning] local frame size: 2000000 bytes (23% of stack limit)
    char buf[2000000];
    
    // at line 42, column 4
    // [!Warning] writing to user-supplied pointer (potential escape)
    if (user_ptr)
        *user_ptr = 42;
    
    buf[0] = 0;
}
