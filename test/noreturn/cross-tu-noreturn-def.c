// SPDX-License-Identifier: Apache-2.0
//
// Error functions for cross-tu-noreturn-use.c (#153). fail() only calls a noreturn function
// and fail_twice() only calls fail(): neither returns. fail_maybe() returns when its flag is 0.

__attribute__((noreturn)) void raise_error(const char* message);

int fail(const char* message)
{
    raise_error(message);
    return 0;
}

int fail_twice(const char* message)
{
    return fail(message);
}

int fail_maybe(int fatal, const char* message)
{
    if (fatal)
        raise_error(message);
    return 0;
}
