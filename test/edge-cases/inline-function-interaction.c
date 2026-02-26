#include <stddef.h>

// Test how inline functions affect stack analysis

static inline void inline_small_stack(void) {
    char buf[256];
    buf[0] = 0;
}

static inline void inline_medium_stack(void) {
    char buf[100000];
    buf[0] = 0;
}

static inline void inline_large_stack(void) {
    char buf[1000000];
    buf[0] = 0;
}

// Non-inline function for reference
void normal_function(void) {
    char buf[500000];
    buf[0] = 0;
}

// at line 27, column 0
// [!Info] function 'caller_of_single_inline' calls inline function
//        ↳ inlined stack from 'inline_small_stack': 256 bytes
//        ↳ local stack: 512 bytes
//        ↳ total: 768 bytes
void caller_of_single_inline(void) {
    char buf[512];
    inline_small_stack();
    buf[0] = 0;
}

// at line 37, column 0
// [!Info] function 'caller_of_multiple_inlines' calls multiple inline functions
//        ↳ inlined stack from 'inline_small_stack': 256 bytes
//        ↳ inlined stack from 'inline_medium_stack': 100000 bytes
//        ↳ inlined stack from 'inline_large_stack': 1000000 bytes
//        ↳ local stack: 512 bytes
//        ↳ max stack including inlined: 1100512 bytes
void caller_of_multiple_inlines(void) {
    char buf[512];
    inline_small_stack();
    inline_medium_stack();
    inline_large_stack();
    buf[0] = 0;
}

// at line 53, column 0
// [!Info] function 'mixed_inline_and_normal' calls both inline and normal functions
//        ↳ inlined from 'inline_small_stack': 256 bytes
//        ↳ call to 'normal_function': 500000 bytes
//        ↳ local stack: 256 bytes
//        ↳ max stack: 500256 bytes (likely inlining vs calling)
void mixed_inline_and_normal(void) {
    char buf[256];
    inline_small_stack();
    normal_function();
    buf[0] = 0;
}
