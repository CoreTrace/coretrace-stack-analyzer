// SPDX-License-Identifier: Apache-2.0
// Another file stores the function where a non-const parameter is expected: with the suggested
// type, this file no longer compiles.
#include "cross-tu-const-suggestion-api.h"

int (*const value_reader)(int*) = read_value;
