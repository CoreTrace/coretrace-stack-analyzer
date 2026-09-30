// SPDX-License-Identifier: Apache-2.0
//
// Defines read_only(const int*) only (#157). Its overload read_only(int*), which
// cross-tu-const-param-overload-use.cpp also calls, is another symbol.

int read_only(const int* p)
{
    return *p;
}
