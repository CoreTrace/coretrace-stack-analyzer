// SPDX-License-Identifier: Apache-2.0
//
// A constructor that passes this to an unknown callback (#179). The callback may keep it: it must
// be reported on every target, also on ELF ones, where the call goes through an alias (C1 -> C2).

struct Resolver;
using Hook = void (*)(Resolver*);

struct Resolver
{
    Resolver(int value, Hook hook);
    int seed;
};

Resolver::Resolver(int value, Hook hook) : seed(value)
{
    hook(this);
}

int run(int value, Hook hook)
{
    const Resolver resolver(value, hook);
    return resolver.seed;
}

// at line 22, column 20
// [ !!Warn ] stack pointer escape: address of variable 'resolver' escapes this function
//          ↳ address passed as argument to function 'Resolver::Resolver(int, void (*)(Resolver*))' (callee may capture the pointer beyond this function)

// strict-expectation-details: true
