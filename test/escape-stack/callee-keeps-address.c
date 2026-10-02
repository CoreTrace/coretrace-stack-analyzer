// SPDX-License-Identifier: Apache-2.0
//
// The address of a local reaches a function defined in this file that keeps it beyond the call
// (#180), itself or through the functions it calls. Each case is reported, as when the caller does
// the same itself. interproc-escape.c stores it into a global directly.

typedef void (*hook_fn)(char*);

struct holder
{
    char* p;
};

static char* g_ptr;
static const char* g_view;
static struct holder g_holder;

static void store_global(char* p)
{
    g_ptr = p;
}

static void call_hook(char* p, hook_fn hook)
{
    hook(p);
}

// A chain: forward_to_store passes p on to store_global.
static void forward_to_store(char* p)
{
    store_global(p);
}

// A permutation: p is the first parameter here, and the second one of store_second.
static void store_second(int n, char* p)
{
    if (n > 0)
        store_global(p);
}

static void forward_swapped(char* p, int n)
{
    store_second(n, p);
}

// A cycle: ping and pong call each other, and ping stores p when n reaches 0.
static void pong(char* p, int n);

static void ping(char* p, int n)
{
    if (n == 0)
        store_global(p);
    else
        pong(p, n - 1);
}

static void pong(char* p, int n)
{
    ping(p, n);
}

// Stores p into the struct h points to. The callers below pass a global struct, then a struct
// of their own caller.
static void attach(struct holder* h, char* p)
{
    h->p = p;
}

static const char* identity(const char* p)
{
    return p;
}

void escape_via_callback(hook_fn hook)
{
    char buf[10] = {0};
    call_hook(buf, hook);
}

void escape_via_chain(void)
{
    char buf[10] = {0};
    forward_to_store(buf);
}

void escape_via_permutation(void)
{
    char buf[10] = {0};
    forward_swapped(buf, 1);
}

void escape_via_cycle(void)
{
    char buf[10] = {0};
    pong(buf, 3);
}

void escape_into_global_holder(void)
{
    char buf[10] = {0};
    attach(&g_holder, buf);
}

void escape_into_caller_param(struct holder* out)
{
    char buf[10] = {0};
    attach(out, buf);
}

// identity returns the address of buf, which the caller then stores into a global.
void escape_after_return(void)
{
    char buf[10] = {0};
    g_view = identity(buf);
}

// identity returns the address of buf, which the caller returns in turn.
const char* return_after_return(void)
{
    char buf[10] = {0};
    return identity(buf);
}

// at line 51, column 9
// [ !Info! ] recursive or mutually recursive function detected

// at line 59, column 10
// [ !Info! ] recursive or mutually recursive function detected

// at line 77, column 5
// [ !!Warn ] stack pointer escape: address of variable 'buf' escapes this function
//          ↳ address passed as argument to function 'call_hook' (callee may capture the pointer beyond this function)

// at line 83, column 5
// [ !!Warn ] stack pointer escape: address of variable 'buf' escapes this function
//          ↳ address passed as argument to function 'forward_to_store' (callee may capture the pointer beyond this function)

// at line 89, column 5
// [ !!Warn ] stack pointer escape: address of variable 'buf' escapes this function
//          ↳ address passed as argument to function 'forward_swapped' (callee may capture the pointer beyond this function)

// at line 95, column 5
// [ !!Warn ] stack pointer escape: address of variable 'buf' escapes this function
//          ↳ address passed as argument to function 'pong' (callee may capture the pointer beyond this function)

// at line 101, column 5
// [ !!Warn ] stack pointer escape: address of variable 'buf' escapes this function
//          ↳ address passed as argument to function 'attach' (callee may capture the pointer beyond this function)

// at line 107, column 5
// [ !!Warn ] stack pointer escape: address of variable 'buf' escapes this function
//          ↳ address passed as argument to function 'attach' (callee may capture the pointer beyond this function)

// at line 114, column 12
// [ !!Warn ] stack pointer escape: address of variable 'buf' escapes this function
//          ↳ stored into global variable 'g_view' (pointer may be used after the function returns)

// at line 121, column 5
// [ !!Warn ] stack pointer escape: address of variable 'buf' escapes this function
//          ↳ escape via return statement (pointer to stack returned to caller)

// strict-expectation-details: true
