// SPDX-License-Identifier: Apache-2.0
// Another file declares the function itself, without the header: rewriting the header would
// leave this declaration with the old type.
int read_value(int* p);

int read_through_own_declaration(int* q)
{
    return read_value(q);
}
