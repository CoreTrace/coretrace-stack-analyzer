// SPDX-License-Identifier: Apache-2.0
//
// Calls probe::peek(int*, std::__cxx11::tag*) (#157), mangled _ZN5probe4peekEPiPNSt7__cxx113tagE.
// cross-tu-uninitialized-abi-def.cpp defines another symbol, for std::__1: its summary must not
// apply here, and the unknown void function stays presumed to write through p.

namespace std
{
    inline namespace __cxx11
    {
        struct tag;
    }
} // namespace std

namespace probe
{
    void peek(int* p, std::tag* t);
} // namespace probe

int uses_other_abi()
{
    int v;
    probe::peek(&v, nullptr);
    return v;
}
