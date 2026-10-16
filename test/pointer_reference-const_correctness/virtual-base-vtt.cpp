// SPDX-License-Identifier: Apache-2.0
// The base-object constructor (C2) of a class with a virtual base takes a vtt after `this`: a
// parameter that the source does not declare and the debug type of the function does not list.
// ConstParameterNotModified must not report it, nor give a parameter after it the type of the
// next one, in that constructor or in the constructors that call it.

struct Base
{
    int seed;
};

struct Reader : virtual Base
{
    Reader(int* x, double* y);
};

// x is only read: both variants, C1 and C2, report it with its own type.
// not contains: current type: double *x
// at line 24, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'x' in function 'Reader::Reader(int*, double*)' is never used to modify the pointed object
//          ↳ current type: int *x
//          ↳ suggested type: const int *x
Reader::Reader(int* x, double* y)
{
    seed = *x;
    *y = 0;
}

// Writer writes through p, so Leaf, which passes its own p there, must not report it.
struct Writer : virtual Base
{
    Writer(int* p, const double* q);
};

struct Leaf : Writer
{
    Leaf(int* p, const double* q);
};

Writer::Writer(int* p, const double* q)
{
    *p = static_cast<int>(*q);
}

// not contains: parameter 'p' in function 'Leaf::Leaf(int*, double const*)'
Leaf::Leaf(int* p, const double* q) : Writer(p, q) {}

// not contains: parameter 'vtt'
