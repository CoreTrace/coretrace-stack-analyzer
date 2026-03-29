// SPDX-License-Identifier: Apache-2.0
#include <string>
#include <vector>

struct Leaf
{
    std::vector<std::string> items;
    int count = 0;
};

struct Middle
{
    Leaf leaf;
    bool active = false;
};

struct Root
{
    int& ref;
    Middle mid; // value-initialized via aggregate init
};

Middle nested_aggregate_copy(int& r)
{
    Root root{r};    // ref = r, mid = value-initialized (nested default ctors)
    return root.mid; // copies Middle -> reads Leaf -> reads vector internals
}

// not contains: potential read of uninitialized local variable 'root'
// not contains: local variable 'root' is never initialized
