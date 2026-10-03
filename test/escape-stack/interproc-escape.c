// SPDX-License-Identifier: Apache-2.0
//
// The address of a local passed to a function defined in this file that stores it into a global
// (#180): it escapes, and is reported at the call.

static char* g_ptr;

static void store_global(char* p)
{
    g_ptr = p;
}

void escape_via_defined_callee(void)
{
    char buf[10] = {0};
    store_global(buf);
}

// at line 16, column 5
// [ !!Warn ] stack pointer escape: address of variable 'buf' escapes this function
//          ↳ address passed as argument to function 'store_global' (callee may capture the pointer beyond this function)

// strict-expectation-details: true
