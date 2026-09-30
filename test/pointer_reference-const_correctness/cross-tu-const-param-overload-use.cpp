// SPDX-License-Identifier: Apache-2.0
//
// Calls two overloads of read_only() (#157). With cross-tu-const-param-overload-def.cpp, only the
// call to read_only(const int*) reaches a definition whose parameter points to const: the fact of
// one overload never reaches the calls to the other.

int read_only(const int* p);
int read_only(int* p);

int uses_const_overload(int* p)
{
    return read_only(static_cast<const int*>(p));
}

int uses_other_overload(int* p)
{
    return read_only(p);
}
