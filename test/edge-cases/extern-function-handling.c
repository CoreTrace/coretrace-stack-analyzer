#include <stdio.h>

// Test how extern functions (declarations without definitions) are handled
// These functions are defined in external libraries, so stack usage is unknown

// External function - stack usage unknown to this compilation unit
extern int external_math_function(int x);

// External function with void return
extern void external_lib_setup(void);

// External function that may use stack
extern char* external_string_operation(const char* input);

// at line 20, column 0
// [!Info] function 'calls_external_math' calls external function
//        ↳ called function: external_math_function
//        ↳ stack propagation: unknown (not in current compilation unit)
//        ↳ conservative stack estimate: 0 bytes from callee
int calls_external_math(int val) {
    int result = external_math_function(val);
    return result;
}

// at line 29, column 0
// [!Info] function 'calls_external_void' calls external void function
//        ↳ called function: external_lib_setup
//        ↳ stack propagation: unknown (external implementation)
void calls_external_void(void) {
    external_lib_setup();
}

// at line 38, column 0
// [!Info] function 'calls_external_string' calls external function returning pointer
//        ↳ called function: external_string_operation
//        ↳ stack propagation: unknown (external definition)
//        ↳ potential note: returned pointer may point to stack (if external uses alloca)
char* calls_external_string(const char* str) {
    char* result = external_string_operation(str);
    return result;  // May point to external stack - conservative warning
}

// at line 45, column 0
// [!Info] function 'mixed_external_calls' combines internal and external calls
void mixed_external_calls(int x) {
    char buf[1024];
    
    int ext_result = external_math_function(x);
    external_lib_setup();
    char* str_result = external_string_operation("test");
    
    buf[0] = 0;
}
