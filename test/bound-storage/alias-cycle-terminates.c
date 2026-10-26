// SPDX-License-Identifier: Apache-2.0
//
// Eight pointers in a chain from one buffer, then each assigned from every other one under a
// condition: one strongly connected component where only `p0` points to the array directly.
// The paths between them number 7! = 5040, and each one used to cost a scan of the function:
// StackBufferOverflow and MultipleStores took hours on such functions (#215, zlib crc32.c).
// The last pointer is chosen by a conditional operator, which is a PHI of pointers in the IR.

void cycle(int n)
{
    char buf[16] = {0};
    char* p0 = buf;
    char* p1 = p0;
    char* p2 = p1;
    char* p3 = p2;
    char* p4 = p3;
    char* p5 = p4;
    char* p6 = p5;
    char* p7 = p6;
    if (n == 1)
        p0 = p1;
    if (n == 2)
        p0 = p2;
    if (n == 3)
        p0 = p3;
    if (n == 4)
        p0 = p4;
    if (n == 5)
        p0 = p5;
    if (n == 6)
        p0 = p6;
    if (n == 7)
        p0 = p7;
    if (n == 8)
        p1 = p0;
    if (n == 10)
        p1 = p2;
    if (n == 11)
        p1 = p3;
    if (n == 12)
        p1 = p4;
    if (n == 13)
        p1 = p5;
    if (n == 14)
        p1 = p6;
    if (n == 15)
        p1 = p7;
    if (n == 16)
        p2 = p0;
    if (n == 17)
        p2 = p1;
    if (n == 19)
        p2 = p3;
    if (n == 20)
        p2 = p4;
    if (n == 21)
        p2 = p5;
    if (n == 22)
        p2 = p6;
    if (n == 23)
        p2 = p7;
    if (n == 24)
        p3 = p0;
    if (n == 25)
        p3 = p1;
    if (n == 26)
        p3 = p2;
    if (n == 28)
        p3 = p4;
    if (n == 29)
        p3 = p5;
    if (n == 30)
        p3 = p6;
    if (n == 31)
        p3 = p7;
    if (n == 32)
        p4 = p0;
    if (n == 33)
        p4 = p1;
    if (n == 34)
        p4 = p2;
    if (n == 35)
        p4 = p3;
    if (n == 37)
        p4 = p5;
    if (n == 38)
        p4 = p6;
    if (n == 39)
        p4 = p7;
    if (n == 40)
        p5 = p0;
    if (n == 41)
        p5 = p1;
    if (n == 42)
        p5 = p2;
    if (n == 43)
        p5 = p3;
    if (n == 44)
        p5 = p4;
    if (n == 46)
        p5 = p6;
    if (n == 47)
        p5 = p7;
    if (n == 48)
        p6 = p0;
    if (n == 49)
        p6 = p1;
    if (n == 50)
        p6 = p2;
    if (n == 51)
        p6 = p3;
    if (n == 52)
        p6 = p4;
    if (n == 53)
        p6 = p5;
    if (n == 55)
        p6 = p7;
    if (n == 56)
        p7 = p0;
    if (n == 57)
        p7 = p1;
    if (n == 58)
        p7 = p2;
    if (n == 59)
        p7 = p3;
    if (n == 60)
        p7 = p4;
    if (n == 61)
        p7 = p5;
    if (n == 62)
        p7 = p6;
    char* q = n < 32 ? p0 : p1;

    // at line 140, column 15
    // [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 16)
    //          ↳ alias path: buf -> arraydecay -> p0
    //          ↳ index variable may go up to 19 (array last valid index: 15)
    //          ↳ (this is a write access)
    for (int i = 0; i < 20; ++i)
        p0[i] = 1;

    // at line 148, column 14
    // [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 16)
    //          ↳ alias path: buf -> arraydecay -> p0 -> cond -> q
    //          ↳ index variable may go up to 19 (array last valid index: 15)
    //          ↳ (this is a write access)
    for (int i = 0; i < 20; ++i)
        q[i] = 1;
}

int main(void)
{
    cycle(3);
    return 0;
}
