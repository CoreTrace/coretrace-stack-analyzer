// SPDX-License-Identifier: Apache-2.0
// k leaves the first loop with a value only its type bounds. The second loop starts from it,
// so the pilot adds nothing there.
void index_from_previous_loop(const int* more)
{
    char buf[4];
    int k = 0;
    while (*more++)
        k++;
    while (k < 4)
        buf[k++] = 'a';
}

// strict-expectation-details: true

// not contains: buffer overflow
// not contains: negative index
