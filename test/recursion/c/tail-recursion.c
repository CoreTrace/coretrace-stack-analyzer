#include <stdio.h>

// Test tail recursion patterns
// Tail recursion can be optimized to iteration by compilers

int sum_tail_recursive(int n, int acc) {
    if (n == 0) {
        return acc;
    }
    return sum_tail_recursive(n - 1, acc + n);
}

// at line 7, column 9
// [ !Info! ] recursive or mutually recursive function detected

void count_down_tail(int n) {
    printf("%d\n", n);
    if (n > 0) {
        count_down_tail(n - 1);  // Tail call - optimizable
    }
}

// at line 17, column 20
// [ !Info! ] recursive or mutually recursive function detected

void factorial_accumulator(int n, long long acc) {
    if (n <= 1) {
        printf("Result: %lld\n", acc);
        return;
    }
    factorial_accumulator(n - 1, acc * n);  // Tail call
}

// at line 27, column 9
// [ !Info! ] recursive or mutually recursive function detected

// Not tail recursive - work after recursive call
int fibonacci(int n) {
    if (n <= 1) {
        return n;
    }
    // Not tail recursive - computation happens after returns
    return fibonacci(n - 1) + fibonacci(n - 2);
}

// at line 37, column 9
// [ !Info! ] recursive or mutually recursive function detected
