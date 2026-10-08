// SPDX-License-Identifier: Apache-2.0
//
// A constructor and a destructor defined out of their class (#179). On ELF targets, clang calls
// them through aliases (C1 -> C2, D1 -> D2). The calls are direct, and every target must agree:
// the max stack of frame() and use() is known, use() reads v uninitialized, frame() reads a
// member that the constructor sets, and box does not escape.

struct Box
{
    explicit Box(const int* out);
    ~Box();
    int value;
};

Box::Box(const int* out) : value(0)
{
    (void)out;
}

Box::~Box() {}

int use()
{
    int v;
    Box box(&v);
    return v + box.value;
}

int frame()
{
    Box box(nullptr);
    return box.value;
}

// at line 26, column 12
// [ !!Warn ] potential read of uninitialized local variable 'v'
//          ↳ this load may execute before any definite initialization on all control-flow paths
