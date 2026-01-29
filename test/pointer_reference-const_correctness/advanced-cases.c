#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>

void consume_const(const int *p) {
    if (p) {
        (void)*p;
    }
}

void consume_mut(int *p) {
    if (p) {
        *p = 1;
    }
}

// at line 21, column 0
// [!]ConstParameterNotModified.Pointer: parameter 'p' in function 'caller_const' is never used to modify the pointed object
//     current type: int *p
//     suggested type: const int *p
void caller_const(int *p) {
    consume_const(p);
}

void caller_mut(int *p) {
    consume_mut(p);
}

// at line 33, column 0
// [!]ConstParameterNotModified.Pointer: parameter 'p' in function 'read_only' is never used to modify the pointed object
//     current type: int *p
//     suggested type: const int *p
void read_only(int *p) {
    int v = *p;
    printf("%d\n", v);
}

void variadic_use(int *p) {
    printf("%p\n", (void *)p);
}

// at line 46, column 0
// [!]ConstParameterNotModified.Pointer: parameter 'reg' in function 'read_mmio' is never used to modify the pointed object
//     current type: volatile int *reg
//     suggested type: const volatile int *reg
void read_mmio(volatile int *reg) {
    int v = *reg;
    (void)v;
}

void takes_double(int **pp) {
    if (pp) {
        (void)*pp;
    }
}

void takes_void(void *p) {
    (void)p;
}
