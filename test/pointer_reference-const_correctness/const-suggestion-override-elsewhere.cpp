// SPDX-License-Identifier: Apache-2.0
// #217: with const, Derived::read no longer overrides Base::read: it becomes another virtual
// function. The using-declaration keeps -Woverloaded-virtual silent, and the key functions are
// defined in another file, so this one emits no vtable and no call: its code changes only the
// method's symbol. The methods it overrides change.

// not contains: parameter 'p' in function 'Derived::read(int*)'
struct Base
{
    virtual ~Base();
    virtual int read(int* p);
};

struct Derived : Base
{
    ~Derived() override;
    using Base::read;
    virtual int read(int* p);
};

int Derived::read(int* p)
{
    return *p;
}
