// SPDX-License-Identifier: Apache-2.0
typedef void (*cb_t)(char*);

static void safe_cb(char* p)
{
    (void)p;
}

cb_t choose_safe(void)
{
    return safe_cb;
}

void call_unknown_callback(cb_t cb)
{
    char buf[16];
    cb(buf);
}

// at line 16, column 5
// [ !!Warn ] stack pointer escape: address of variable 'buf' escapes this function
//          ↳ address passed as argument to an indirect call (callback may capture the pointer beyond this function)

// at line 15, column 1
// [ !!Warn ] local variable 'buf' is never initialized
