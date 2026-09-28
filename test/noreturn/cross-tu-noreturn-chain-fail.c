// SPDX-License-Identifier: Apache-2.0
//
// First link of a chain over three files (#153): fail() only calls a noreturn function.

__attribute__((noreturn)) void raise_error(const char* message);

int fail(const char* message)
{
    raise_error(message);
    return 0;
}
