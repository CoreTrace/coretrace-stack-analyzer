// SPDX-License-Identifier: Apache-2.0
// #217: the const version compiles, but the generated code changes: a call, an override or a
// constant now resolves elsewhere. The parameter is not reported.

// strict-expectation-details: true
// not contains: parameter 'p' in function 'overload_target(int*)'
// not contains: parameter 'p' in function 'Shape::area(int*)'
// not contains: parameter 'p' in function 'ReadDerived::read(int*)'
// not contains: parameter 'p' in function 'ReadBase::read(int*)'
// not contains: parameter 'p' in function 'sized_target(int*)'
// not contains: parameter 'p' in function 'branch_target(int*)'
// not contains: parameter 'p' in function 'self_typed(int*)'
#include <type_traits>

// A caller with a pointer to const now calls this overload instead of the bool one.
int overload_target(int* p)
{
    return *p;
}

int overload_target(bool flag)
{
    return flag ? 1 : 0;
}

int call_overload(const int* q)
{
    return overload_target(q);
}

// With const, ReadDerived::read no longer overrides ReadBase::read. The using-declaration keeps
// -Woverloaded-virtual silent: nothing warns.
struct ReadBase
{
    virtual ~ReadBase() = default;
    virtual int read(int* p)
    {
        return *p;
    }
};

struct ReadDerived : ReadBase
{
    using ReadBase::read;
    int read(int* p)
    {
        return *p;
    }
};

int call_read(ReadBase& object, int* p)
{
    return object.read(p);
}

ReadBase base_reader;
ReadDerived derived_reader;

int make_read(int* p)
{
    return call_read(derived_reader, p);
}

// A virtual call now selects another overload: only its vtable slot and argument type change.
// Shape::area(int*) is defined out of line: it is the key function, the vtable is emitted here.
struct Shape
{
    virtual ~Shape() = default;
    virtual int area(bool flag)
    {
        return flag ? 1 : 0;
    }
    virtual int area(int* p);
};

int Shape::area(int* p)
{
    return *p;
}

int call_area(Shape& shape, const int* q)
{
    return shape.area(q);
}

// sizeof selects an overload without calling it: the returned constant changes.
int sized_target(int* p)
{
    return *p;
}

char sized_target(bool flag);

int call_sized(const int* q)
{
    return static_cast<int>(sizeof(sized_target(q)));
}

// if constexpr chooses its branch from an unevaluated call.
int branch_target(int* p)
{
    return *p;
}

char branch_target(bool flag);

template <class T> int pick_branch(T q)
{
    if constexpr (sizeof(branch_target(q)) == 1)
        return 10;
    else
        return 20;
}

int call_branch(const int* q)
{
    return pick_branch(q);
}

// The function inspects its own parameter's type.
int self_typed(int* p)
{
    if constexpr (std::is_const_v<std::remove_pointer_t<decltype(p)>>)
        return 1;
    else
        return *p;
}

// Controls: the code stays the same.
// at line 136, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'called_directly(int*)' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
// ↳ ABI: the suggested type changes the symbol (_Z15called_directlyPi -> _Z15called_directlyPKi): code compiled separately must use the updated declaration and be recompiled
int called_directly(int* p)
{
    return *p;
}

int call_directly(int* p)
{
    return called_directly(p);
}

// at line 152, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'twice_target(int*)' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
// ↳ ABI: the suggested type changes the symbol (_Z12twice_targetPi -> _Z12twice_targetPKi): code compiled separately must use the updated declaration and be recompiled
int twice_target(int* p)
{
    return *p;
}

int twice_target(bool flag)
{
    return flag ? 2 : 3;
}

#define TWICE(x) (twice_target(x) == twice_target(true))

int call_twice(int* p)
{
    return TWICE(p);
}
