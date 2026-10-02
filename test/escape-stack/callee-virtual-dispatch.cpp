// SPDX-License-Identifier: Apache-2.0
//
// A method defined in this file passes the address of a local, received as a parameter, to a
// virtual call (#180). Its candidate targets decide: Store::keep keeps it in a global, and the call
// is reported; Store::read only reads through it, and nothing is reported.

static char* kept;
static int last;

struct Sink
{
    virtual void keep(char* p) = 0;
    virtual void read(const char* p) = 0;
};

struct Store final : Sink
{
    void keep(char* p) override
    {
        kept = p;
    }
    void read(const char* p) override
    {
        last = p[0];
    }
};

struct Feeder
{
    Sink* sink;
    void feedKeep(char* p)
    {
        sink->keep(p);
    }
    void feedRead(const char* p)
    {
        sink->read(p);
    }
};

void escape_via_virtual(Feeder& feeder)
{
    char buf[4] = {0};
    feeder.feedKeep(buf);
}

int reads_via_virtual(Feeder& feeder)
{
    char buf[4] = {0};
    feeder.feedRead(buf);
    return last;
}

Sink* make()
{
    static Store store;
    return &store;
}

// at line 44, column 12
// [ !!Warn ] stack pointer escape: address of variable 'buf' escapes this function
//          ↳ address passed as argument to function 'Feeder::feedKeep(char*)' (callee may capture the pointer beyond this function)

// strict-expectation-details: true
