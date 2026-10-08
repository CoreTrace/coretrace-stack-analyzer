// SPDX-License-Identifier: Apache-2.0
//
// The object built by an out-of-class constructor of the same file only reaches that constructor
// and a reader (#179). On ELF targets, clang calls the constructor through an alias (C1 -> C2):
// the call is direct, and the address does not escape, on every target.

struct Resolver
{
    explicit Resolver(int value);
    int seed;
};

Resolver::Resolver(int value) : seed(value) {}

static int lookup(const Resolver* resolver)
{
    return resolver->seed;
}

int run(int value)
{
    const Resolver resolver(value);
    return lookup(&resolver);
}
