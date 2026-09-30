// SPDX-License-Identifier: Apache-2.0
//
// Caller of read_only() (#157). Alone, read_only() is a declaration without its definition: p may
// be written through it, and nothing is reported. With cross-tu-const-param-def.c, whose
// read_only() takes a pointer to const, p could point to const, as within one file. run_test.py
// checks each pairing.

int read_only(const int* p);

int uses_read_only(int* p)
{
    return read_only(p);
}
