#include <string.h>

// Test cases for buffer underflow and negative index access

// at line 7, column 0
// [!!!Error] function 'negative_index_write' accesses buffer with negative index
//          ↳ buffer 'buf' accessed at negative offset
//          ↳ indicates potential stack buffer underflow
void negative_index_write(void) {
    char buf[256];
    char* p = &buf[10];
    
    // at line 14, column 4
    // [!!!Error] writing at negative offset from pointer
    //          ↳ writing to address below buffer start
    p[-11] = 'X';  // Access at buf[-1] - UNDERFLOW
}

// at line 19, column 0
// [!!!Error] function 'underflow_via_pointer_arithmetic' uses negative arithmetic
void underflow_via_pointer_arithmetic(void) {
    char buf[256];
    char* p = &buf[5];
    
    // at line 25, column 4
    // [!!!Error] pointer arithmetic creates address below buffer
    //          ↳ effective address: &buf[-5]
    p[-10] = 0;  // Underflow via pointer arithmetic
}

// at line 30, column 0
// [!!!Error] function 'underflow_via_memcpy' copies with negative offset
void underflow_via_memcpy(void) {
    char buf[256];
    char* p = &buf[5];
    char src[10] = "test";
    
    // at line 37, column 4
    // [!!!Error] memcpy destination below buffer start
    //          ↳ destination: &buf[-5] (10 bytes before allocation)
    //          ↳ may corrupt data below this buffer
    memcpy(p - 10, src, 20);  // Copies past start of buffer
}

// at line 44, column 0
// [!Warning] function 'boundary_underflow' accesses exactly at boundary
void boundary_underflow(void) {
    char buf[256];
    char* p = buf;
    
    // at line 50, column 4
    // [!!!Error] accessing one byte before buffer start
    //          ↳ this byte belongs to previous allocation
    p[-1] = 0;
}
