// SPDX-License-Identifier: Apache-2.0
static void redirect_ptr(int** out, int* target)
{
    *out = target;
}

int read_after_pointer_redirect(void)
{
    int uninit;
    int init = 42;
    int* ptr = &uninit;
    redirect_ptr(&ptr, &init);
    return *ptr;
}

// not contains: potential read of uninitialized local variable 'uninit'

// at line 8, column 1
// [ !!Warn ] local variable 'uninit' is never initialized
// ↳ declared without initializer and no definite write was found in this function
