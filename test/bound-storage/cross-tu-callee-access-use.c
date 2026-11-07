// SPDX-License-Identifier: Apache-2.0
//
// Callers of functions defined in other files (#266). Alone, they are declarations, and nothing
// is reported. With cross-tu-callee-access-leaf.c, a write or a read one byte past buf is
// reported at the call, as within one file; with cross-tu-callee-access-mid.c too, so is the
// write through relay(). run_test.py checks each pairing.

void write_first(char* p);
char read_first(const char* p);
void relay(char* p);

void write_past_end(void)
{
    char buf[8] = {0};
    write_first(buf + 8);
}

void write_last_byte(void)
{
    char buf[8] = {0};
    write_first(buf + 7);
}

char read_past_end(void)
{
    char buf[8] = {0};
    return read_first(buf + 8);
}

void write_through_relay(void)
{
    char buf[8] = {0};
    char* p = &buf[8];
    relay(p);
}
