// SPDX-License-Identifier: Apache-2.0
//
// probe::peek(int*, std::__1::tag*) (#157), mangled _ZN5probe4peekEPiPNSt3__13tagE. It writes
// nothing through p. cross-tu-uninitialized-abi-use.cpp calls probe::peek(int*,
// std::__cxx11::tag*), _ZN5probe4peekEPiPNSt7__cxx113tagE: another ABI symbol, although
// canonicalizeMangledName gives both the same name.

namespace std
{
    inline namespace __1
    {
        struct tag;
    }
} // namespace std

namespace probe
{
    void peek(int* p, std::tag* t)
    {
        (void)p;
        (void)t;
    }
} // namespace probe

// at line 18, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'probe::peek(int*, std::__1::tag*)' is never used to modify the pointed object
//             ↳ current type: int *p
//             ↳ suggested type: const int *p

// at line 18, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 't' in function 'probe::peek(int*, std::__1::tag*)' is never used to modify the pointed object
//             ↳ current type: tag *t
//             ↳ suggested type: const tag *t
