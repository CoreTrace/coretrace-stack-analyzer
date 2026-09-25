// SPDX-License-Identifier: Apache-2.0
// Input of testDuplicateIfIndexWidths, which rewrites the index of the second condition to
// another integer width.

int same_element_twice(const int* p)
{
    if (p[-1] == 3)
        return 1;
    else if (p[-1] == 3)
        return 2;
    return 0;
}
