// SPDX-License-Identifier: Apache-2.0
// Parameters for resolveParameterDebugBinding: a free function, a method, the constructors of a
// class with a virtual base, and a method with an explicit object parameter. Compiled with
// -std=c++23, once for each C++ ABI: under the Itanium ABI, the base-object constructor (C2)
// takes a vtt after `this`; under the Microsoft ABI, the constructor takes is_most_derived last.

struct Base
{
    int seed;
};

struct Reader : virtual Base
{
    Reader(int* x, double* y);
    long sum(int* p, double* q);
};

Reader::Reader(int* x, double* y)
{
    seed = *x + static_cast<int>(*y);
}

long Reader::sum(int* p, double* q)
{
    return *p + static_cast<long>(*q);
}

long free_function(int* a, double* b)
{
    return *a + static_cast<long>(*b);
}

// An explicit object parameter is declared in the source: p is the second parameter.
struct Picker
{
    int pick(this const Picker& self, int* p)
    {
        return *p;
    }
};

int use_picker(int* p)
{
    return Picker{}.pick(p);
}

// A variadic constructor: the debug type of the function ends with a null entry for `...`, which
// no parameter takes. Under the Microsoft ABI, is_most_derived comes second.
struct Varargs : virtual Base
{
    Varargs(int* x, double* y, ...);
};

Varargs::Varargs(int* x, double* y, ...)
{
    seed = *x + static_cast<int>(*y);
}

// An empty parameter keeps its debug declaration and its number; whether it takes an IR argument
// depends on the ABI: none on x86-64 or Apple arm64, an i8 under AAPCS64 on Linux.
struct Tag
{
};

struct Tagged : virtual Base
{
    Tagged(int* x, Tag tag);
};

Tagged::Tagged(int* x, Tag tag)
{
    (void)tag;
    seed = *x;
}

// A variadic function that returns a struct through a hidden argument.
struct Big
{
    long values[8];
};

Big make_big(int* p, ...)
{
    Big big{};
    big.values[0] = *p;
    return big;
}

// A method that returns a struct through a hidden argument, which the Microsoft ABI puts after
// `this`.
struct Maker
{
    Big make(int* p) const;
};

Big Maker::make(int* p) const
{
    Big big{};
    big.values[0] = *p;
    return big;
}

// The empty parameter comes first: x is the second parameter the source declares.
struct TagFirst : virtual Base
{
    TagFirst(Tag tag, int* x);
};

TagFirst::TagFirst(Tag tag, int* x)
{
    (void)tag;
    seed = *x;
}
