// SPDX-License-Identifier: Apache-2.0
void same_index_store(void)
{
    char buf[10];
    buf[2] = 'a';
    buf[2] = 'b';
}

int main(void)
{
    same_index_store();
    return 0;
}

// at line 3, column 1
// [ !Info! ] multiple stores to stack buffer 'buf' in this function (2 store instruction(s), 1 distinct index expression(s))
// [ !Info! ] all stores use the same index expression (possible redundant or unintended overwrite)
