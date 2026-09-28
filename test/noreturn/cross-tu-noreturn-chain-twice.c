// SPDX-License-Identifier: Apache-2.0
//
// Second link of a chain over three files (#153): fail_twice() only calls fail(), defined in
// cross-tu-noreturn-chain-fail.c.

int fail(const char* message);

int fail_twice(const char* message)
{
    return fail(message);
}
