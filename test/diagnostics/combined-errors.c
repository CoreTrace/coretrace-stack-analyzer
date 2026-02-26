#include <alloca.h>
#include <string.h>

// Test diagnostics combining multiple error types in one function

// at line 8, column 6
// [!!!Error] function 'combined_errors' demonstrates multiple issues
void combined_errors(int user_size) {
    // Issue 1: Large local frame approaching 50% of stack limit
    // at line 12, column 4
    // [!Warning] local frame size: 3000000 bytes
    //          ↳ 35% of stack limit (8388608 bytes)
    char local_large[3000000];
    
    // Issue 2: User-controlled alloca
    // at line 20, column 18
    // [!!!Error] alloca with user-controlled size
    //          ↳ allocation size determined by 'user_size' parameter
    //          ↳ unbounded allocation from untrusted source
    char* user_alloc = (char*)alloca(user_size);
    
    // Issue 3: Writing beyond allocated size
    // at line 27, column 4
    // [!!!Error] buffer overflow via memcpy
    //          ↳ writing 512 bytes into 256-byte buffer
    char small_buf[256];
    memcpy(small_buf, "X", 512);
    
    local_large[0] = 0;
}

// at line 34, column 0
// [!Warning] function 'multiple_warnings' combines warning-level issues
void multiple_warnings(void) {
    // at line 38, column 4
    // [!Warning] large local allocation: 5000000 bytes (59% of limit)
    char buf1[5000000];
    
    // at line 42, column 4
    // [!Warning] second large local allocation: 2000000 bytes (23% of limit)
    //          ↳ combined with buf1: 7000000 bytes (83% of limit)
    char buf2[2000000];
    
    buf1[0] = 0;
    buf2[0] = 0;
}

// at line 50, column 0
// [!!!Error] function 'stack_pointer_escape_and_overflow' combines escape and overflow
char* escaped_ptr = NULL;

char* stack_pointer_escape_and_overflow(void) {
    // at line 57, column 4
    // [!!!Error] local buffer address escapes function
    //          ↳ buffer 'buf' address returned to caller
    //          ↳ becomes invalid after function returns
    char buf[1024];
    
    // at line 62, column 4
    // [!!!Error] potential buffer overflow
    //          ↳ writing 2000 bytes to 1024-byte buffer
    memset(buf, 'A', 2000);
    
    // at line 67, column 11
    // [!!!Error] escaping stack address
    //          ↳ returning address of local buffer
    return buf;
}
