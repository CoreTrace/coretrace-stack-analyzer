// SPDX-License-Identifier: Apache-2.0
//
// Recursions without a way out stay reported (#160). __builtin_unreachable() states that a
// path does not run: it is not a base case.

// Nothing stops the recursion.
void spin(int n)
{
    spin(n + 1);
}

// The only other path is declared impossible.
void descend(int n)
{
    if (n == 0)
        __builtin_unreachable();
    descend(n - 1);
}

// at line 9, column 10
// [!!!Error] unconditional self recursion detected (no base case)
// ↳ this will eventually overflow the stack at runtime

// at line 15, column 9
// [!!!Error] unconditional self recursion detected (no base case)
// ↳ this will eventually overflow the stack at runtime

// strict-expectation-details: true
