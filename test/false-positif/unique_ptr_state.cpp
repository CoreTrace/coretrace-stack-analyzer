// SPDX-License-Identifier: Apache-2.0
static void make_unique_sink(char* p)
{
    (void)p;
}

static void unique_ptr_sink(char* p)
{
    (void)p;
}

static void transition(void)
{
    char buf[8];
    make_unique_sink(buf);
    unique_ptr_sink(buf);
}

int main(void)
{
    transition();
    return 0;
}

// not contains: stack pointer escape

// at line 13, column 1
// [ !!Warn ] local variable 'buf' is never initialized
