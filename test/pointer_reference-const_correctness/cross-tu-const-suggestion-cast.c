// SPDX-License-Identifier: Apache-2.0
// Another file converts the function's address with an explicit cast: the cast would accept the
// new type silently.
#include "cross-tu-const-suggestion-api.h"

typedef int (*erased_reader)(void*);

erased_reader erase_reader(void)
{
    return (erased_reader)read_value;
}
