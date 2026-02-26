#include <stdio.h>
#include <stddef.h>

// Test cases for function pointer calls and indirect invocations

typedef void (*simple_func_ptr)(void);
typedef int (*func_ptr_with_args)(int x);

void simple_callee(void) {
    char buf[512];
    buf[0] = 0;
}

int callee_with_args(int x) {
    char buf[256];
    buf[0] = 0;
    return x * 2;
}

void caller_via_simple_pointer(void) {
    char buf[256];
    simple_func_ptr fp = simple_callee;
    fp();
    buf[0] = 0;
}

int caller_with_args_pointer(int val) {
    char buf[128];
    func_ptr_with_args fp = callee_with_args;
    int result = fp(val);
    buf[0] = 0;
    return result;
}

void multiple_pointer_calls(void) {
    char buf[512];
    simple_func_ptr fp1 = simple_callee;
    func_ptr_with_args fp2 = callee_with_args;
    fp1();
    fp2(10);
    buf[0] = 0;
}


// Key Problems with Function Pointer Analysis
// Call Target Resolution: When you call through a function pointer like fp(), the analyzer must statically determine which functions could be called. In this file:

// fp in caller_via_simple_pointer points to simple_callee (512 bytes)
// fp in caller_with_args_pointer points to callee_with_args (256 bytes)
// Without proper interprocedural analysis, the analyzer might fail to connect the pointer assignment to the actual function being invoked.

// Stack Propagation Through Indirection: Direct calls are straightforward—caller() calls callee(), add their stacks. With pointers:

// caller_via_simple_pointer allocates 256 bytes locally
// It then calls simple_callee via pointer, which allocates 512 bytes
// Total stack: 256 + 512 = 768 bytes
// If the analyzer can't resolve the indirect call, it might only count 256 bytes, missing 512.

// Multiple Pointer Targets: In multiple_pointer_calls:

// fp1 could theoretically point to any function matching the signature
// fp2 could point to any function with that signature
// The analyzer must handle potential aliasing and polymorphism correctly
// Tracking Pointer Assignments: The analyzer must:

// Track that fp = simple_callee at one location
// Remember this binding through the function scope
// Resolve it correctly at the call site fp()
// Real-world Complexity: In actual code, function pointers might be:

// Passed as parameters
// Returned from functions
// Stored in structs
// Set conditionally based on runtime values
// This makes static analysis significantly more difficult than direct calls.