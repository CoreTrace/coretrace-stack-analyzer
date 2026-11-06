// SPDX-License-Identifier: Apache-2.0
// Nested loops: i * 4 + j covers 0..15 of a 16-byte array.
void nested_ok(void)
{
    char buf[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            buf[i * 4 + j] = 'a';
}

// strict-expectation-details: true

// not contains: buffer overflow
