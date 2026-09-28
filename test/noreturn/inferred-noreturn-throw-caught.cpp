// SPDX-License-Identifier: Apache-2.0
//
// fail_throw never returns (#153), but inside try/catch the handler goes on to the write with
// n == 0: the guard protects nothing.

#include <cstring>
#include <stdexcept>

int fail_throw(const char* message)
{
    throw std::runtime_error(message);
}

void copy_caught(char* dst, const char* src, std::size_t n)
{
    try
    {
        if (n < 1)
            fail_throw("empty");
    }
    catch (...)
    {
    }
    std::strncpy(dst, src, n - 1);
}

// at line 24, column 5
// [ !!Warn ] potential unsafe write with length (size - 1) in strncpy
// ↳ size operand may be less than 1

// strict-expectation-details: true
