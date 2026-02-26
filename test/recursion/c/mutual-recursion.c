#include <stdio.h>

// Test mutual recursion - two functions that call each other

// Forward declaration
void func_b(int depth);

void func_a(int n) {
    char buf[256];
    
    if (n > 0) {
        func_b(n - 1);
    }
    
    buf[0] = 0;
}

// at line 11, column 9
// [ !Info! ] recursive or mutually recursive function detected

void func_b(int depth) {
    char buf[512];
    
    if (depth > 0) {
        func_a(depth - 1);
    }
    
    buf[0] = 0;
}

// at line 24, column 9
// [ !Info! ] recursive or mutually recursive function detected

// Three-way cycle test
void func_c(int x);
void func_d(int x);

void func_c(int x) {
    char buf[128];
    if (x > 0)
        func_d(x - 1);
    buf[0] = 0;
}

// at line 40, column 9
// [ !Info! ] recursive or mutually recursive function detected

void func_d(int x) {
    char buf[256];
    if (x > 0)
        func_c(x - 1);
    buf[0] = 0;
}

// at line 49, column 9
// [ !Info! ] recursive or mutually recursive function detected
