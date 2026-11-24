// SPDX-License-Identifier: Apache-2.0
// #217: a class that inherits a constructor (`using Base::Base`) gets constructors of its own,
// whose symbols name the inherited constructor's parameter types: the rewrite renames them too.
// The check pairs them before and after the rewrite through the constructor they inherit, so
// each suggestion is checked: a plain class, a class template as the base, a class template as
// the derived class, and a constructor template, inherited twice, for int* and for double*: the
// derived class inherits two constructors, one from each specialization.

// strict-expectation-details: true

// at line 19, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'Base::Base(int*)' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
struct Base
{
    int v;
    Base(int* p) : v(*p) {}
};

struct Derived : Base
{
    using Base::Base;
    int w = 0;
};

int make_derived(int x)
{
    Derived d(&x);
    return d.v;
}

// at line 42, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'TemplateBase<int>::TemplateBase(int*)' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
template <typename T> struct TemplateBase
{
    T v;
    TemplateBase(T* p) : v(*p) {}
};

struct FromTemplate : TemplateBase<int>
{
    using TemplateBase::TemplateBase;
    int w = 0;
};

int make_from_template(int x)
{
    FromTemplate d(&x);
    return d.v;
}

// at line 65, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'PlainBase::PlainBase(int*)' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
struct PlainBase
{
    int v;
    PlainBase(int* p) : v(*p) {}
};

template <typename T> struct TemplateDerived : PlainBase
{
    using PlainBase::PlainBase;
    T w = 0;
};

int make_template_derived(int x)
{
    TemplateDerived<int> d(&x);
    return d.v;
}

// at line 94, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'GenericBase::GenericBase<int>(int*)' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside

// at line 94, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'GenericBase::GenericBase<double>(double*)' is never used to modify the pointed object
// ↳ current type: double *p
// ↳ suggested type: const double *p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
struct GenericBase
{
    int v;
    template <typename T> GenericBase(T* p) : v(*p) {}
};

struct FromGeneric : GenericBase
{
    using GenericBase::GenericBase;
    int w = 0;
};

int make_from_generic(int x)
{
    FromGeneric d(&x);
    return d.v;
}

int make_from_generic_double(double x)
{
    FromGeneric d(&x);
    return d.v;
}

// The Clang check of const suggestions is experimental: it runs only with this option.
// analyzer-arg: --experimental-const-suggestion-check
