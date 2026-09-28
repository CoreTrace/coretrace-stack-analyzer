// SPDX-License-Identifier: Apache-2.0
//
// A C++ function that always throws never returns, even without [[noreturn]] (#153): the guard
// protects what follows it.

#include <cstring>
#include <stdexcept>

int fail_throw(const char* message)
{
    throw std::runtime_error(message);
}

// n >= 1 below the guard: n - 1 cannot wrap.
void copy_throw(char* dst, const char* src, std::size_t n)
{
    if (n < 1)
        fail_throw("empty");
    std::strncpy(dst, src, n - 1);
}

// not contains: potential unsafe write with length (size - 1)
