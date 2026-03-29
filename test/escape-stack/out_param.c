// SPDX-License-Identifier: Apache-2.0
void leak_out_param(char** out)
{
    char buf[10];

    *out = buf; // leak via out-parameter
}

void safe_out_param(char** out)
{
    char* local = 0; // pointer, but no stack buffer behind it
    *out = local;    // not a stack variable address
}

// at line 3, column 1
// [ !!Warn ] local variable 'buf' is never initialized
//          ↳ declared without initializer and no definite write was found in this function

// at line 5, column 10
// [ !!Warn ] stack pointer escape: address of variable 'buf' escapes this function
//          ↳ stored through a non-local pointer (e.g. via an out-parameter; pointer may outlive this function)
