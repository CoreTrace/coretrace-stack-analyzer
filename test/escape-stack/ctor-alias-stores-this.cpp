// SPDX-License-Identifier: Apache-2.0
//
// A constructor that stores this in a global (#179). The address escapes: the constructor is
// defined in the same file, and keeps it. It must be reported on every target, also on ELF ones,
// where the call goes through an alias (C1 -> C2).

struct Resolver;
Resolver* lastResolver;

struct Resolver
{
    explicit Resolver(int value);
    int seed;
};

Resolver::Resolver(int value) : seed(value)
{
    lastResolver = this;
}

static int lookup(const Resolver* resolver)
{
    return resolver->seed;
}

int run(int value)
{
    const Resolver resolver(value);
    return lookup(&resolver);
}

// at line 28, column 20
// [ !!Warn ] stack pointer escape: address of variable 'resolver' escapes this function
//          ↳ address passed as argument to function 'Resolver::Resolver(int)' (callee may capture the pointer beyond this function)

// strict-expectation-details: true
