// SPDX-License-Identifier: Apache-2.0
//
// A chain of 66 pointers. The safe access through the last one is resolved first; the old
// resolver gave up past 64 pointers, and a resolver that remembered the results computed
// under that cut lost the report on the write through `p1` (#215, first review).

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
    char* p8 = p7;
    char* p9 = p8;
    char* p10 = p9;
    char* p11 = p10;
    char* p12 = p11;
    char* p13 = p12;
    char* p14 = p13;
    char* p15 = p14;
    char* p16 = p15;
    char* p17 = p16;
    char* p18 = p17;
    char* p19 = p18;
    char* p20 = p19;
    char* p21 = p20;
    char* p22 = p21;
    char* p23 = p22;
    char* p24 = p23;
    char* p25 = p24;
    char* p26 = p25;
    char* p27 = p26;
    char* p28 = p27;
    char* p29 = p28;
    char* p30 = p29;
    char* p31 = p30;
    char* p32 = p31;
    char* p33 = p32;
    char* p34 = p33;
    char* p35 = p34;
    char* p36 = p35;
    char* p37 = p36;
    char* p38 = p37;
    char* p39 = p38;
    char* p40 = p39;
    char* p41 = p40;
    char* p42 = p41;
    char* p43 = p42;
    char* p44 = p43;
    char* p45 = p44;
    char* p46 = p45;
    char* p47 = p46;
    char* p48 = p47;
    char* p49 = p48;
    char* p50 = p49;
    char* p51 = p50;
    char* p52 = p51;
    char* p53 = p52;
    char* p54 = p53;
    char* p55 = p54;
    char* p56 = p55;
    char* p57 = p56;
    char* p58 = p57;
    char* p59 = p58;
    char* p60 = p59;
    char* p61 = p60;
    char* p62 = p61;
    char* p63 = p62;
    char* p64 = p63;
    char* p65 = p64;
    p65[0] = n;

    // at line 84, column 15
    // [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 16)
    //          ↳ alias path: buf -> arraydecay -> p0 -> p1
    //          ↳ index variable may go up to 19 (array last valid index: 15)
    //          ↳ (this is a write access)
    for (int i = 0; i < 20; ++i)
        p1[i] = 1;
}

int main(void)
{
    cycle(3);
    return 0;
}

// The safe access through `p65` counts as a store into `buf` too: the resolver no longer gives
// up past 64 pointers.
// at line 9, column 1
// [ !Info! ] multiple stores to stack buffer 'buf' in this function (2 store instruction(s), 2 distinct index expression(s))
// [ !Info! ] stores use different index expressions; verify indices are correct and non-overlapping
