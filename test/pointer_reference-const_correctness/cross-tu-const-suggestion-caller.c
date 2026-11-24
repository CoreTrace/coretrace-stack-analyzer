// SPDX-License-Identifier: Apache-2.0
// Another file calls the function through the header: it compiles with the suggested type.
#include "cross-tu-const-suggestion-api.h"

int call_value(void)
{
    int value = 1;
    return read_value(&value);
}
