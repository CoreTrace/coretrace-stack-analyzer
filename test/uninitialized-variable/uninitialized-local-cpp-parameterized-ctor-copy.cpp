// SPDX-License-Identifier: Apache-2.0
#include <string>
#include <vector>

struct Inner
{
    std::vector<int> data;
    std::string label;
};

struct Outer
{
    int id;
    Inner inner; // value-initialized via aggregate init
};

Inner parameterized_aggregate_init_then_copy(void)
{
    int x = 42;
    Outer obj{x};     // id = x, inner = value-initialized (default ctor)
    return obj.inner; // copy-ctor reads inner
}

// not contains: potential read of uninitialized local variable 'obj'
// not contains: local variable 'obj' is never initialized
