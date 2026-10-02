// SPDX-License-Identifier: Apache-2.0
//
// The address of a local reaches a function defined in this file that does not keep it beyond the
// life of that local (#180). Nothing is reported. Each case states why: a callee that stores the
// address, or returns it, stays safe only because the caller's object lives in the same frame;
// callee-keeps-address.c holds the same stores and returns where it does not.

struct buffer
{
    char* cursor;
    char storage[16];
};

static int read_first(const char* p)
{
    return p[0];
}

static const char* identity(const char* p)
{
    return p;
}

static void buffer_init(struct buffer* b)
{
    b->cursor = b->storage;
}

static void set_slot(char** slot, char* p)
{
    *slot = p;
}

// A cycle that only reads: neither function keeps p.
static int ping_read(const char* p, int n);

static int pong_read(const char* p, int n)
{
    return n == 0 ? p[0] : ping_read(p, n - 1);
}

static int ping_read(const char* p, int n)
{
    return pong_read(p, n);
}

// read_first reads through p and keeps nothing.
int only_reads(void)
{
    char buf[4] = {0};
    return read_first(buf);
}

// identity returns the address of buf, which the caller only reads through before returning.
int returns_it(void)
{
    char buf[4] = {0};
    return identity(buf)[0];
}

// buffer_init stores an address inside b into b: both live in this frame, and end with it.
int stores_into_own_struct(void)
{
    struct buffer b;
    buffer_init(&b);
    return b.cursor == b.storage;
}

// set_slot stores the address of buf into slot, a local of this frame too.
int stores_into_own_slot(void)
{
    char buf[4] = {0};
    char* slot = buf;
    set_slot(&slot, buf);
    return slot[0];
}

int reads_through_cycle(void)
{
    char buf[4] = {0};
    return ping_read(buf, 3);
}

// at line 39, column 12
// [ !Info! ] recursive or mutually recursive function detected

// at line 44, column 22
// [ !Info! ] recursive or mutually recursive function detected

// strict-expectation-details: true
