// SPDX-License-Identifier: Apache-2.0
// The parameter of an implicitly defined move constructor is not one the source declares: nothing
// can make it const, and ConstParameterNotModified must not report it.

struct Member
{
    Member() = default;
    Member(const Member& other) : v(other.v) {}
    int v = 0;
};

struct Holder
{
    Member m;
    Holder() = default;
};

// not contains: in function 'Holder::Holder(Holder&&)'
Holder take(Holder&& h)
{
    Holder out(static_cast<Holder&&>(h));
    return out;
}
