// SPDX-License-Identifier: Apache-2.0
//
// A constructor defined in its class that stores this into a global (#180): the address of
// resolver escapes, and is reported at the construction. No target calls such a constructor
// through an alias.

struct Resolver;
Resolver* lastResolver;

struct Resolver
{
    explicit Resolver(int value) : seed(value)
    {
        lastResolver = this;
    }
    int seed;
};

int run(int value)
{
    const Resolver resolver(value);
    return resolver.seed;
}

// at line 21, column 20
// [ !!Warn ] stack pointer escape: address of variable 'resolver' escapes this function
//          ↳ address passed as argument to function 'Resolver::Resolver(int)' (callee may capture the pointer beyond this function)

// strict-expectation-details: true
