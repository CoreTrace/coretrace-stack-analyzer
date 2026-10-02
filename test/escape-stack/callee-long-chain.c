// SPDX-License-Identifier: Apache-2.0
//
// A chain of 70 functions defined in this file (#180): each one passes the address it receives
// to the next, and link69 stores it into a global. The call to link0 is reported: the routes
// of the parameters are computed until they no longer change, however long the chain.

static char* g_ptr;

static void link69(char* p)
{
    g_ptr = p;
}

// Defines a function that passes p on to next.
#define LINK(name, next)                                                                           \
    static void name(char* p)                                                                      \
    {                                                                                              \
        next(p);                                                                                   \
    }

LINK(link68, link69)
LINK(link67, link68)
LINK(link66, link67)
LINK(link65, link66)
LINK(link64, link65)
LINK(link63, link64)
LINK(link62, link63)
LINK(link61, link62)
LINK(link60, link61)
LINK(link59, link60)
LINK(link58, link59)
LINK(link57, link58)
LINK(link56, link57)
LINK(link55, link56)
LINK(link54, link55)
LINK(link53, link54)
LINK(link52, link53)
LINK(link51, link52)
LINK(link50, link51)
LINK(link49, link50)
LINK(link48, link49)
LINK(link47, link48)
LINK(link46, link47)
LINK(link45, link46)
LINK(link44, link45)
LINK(link43, link44)
LINK(link42, link43)
LINK(link41, link42)
LINK(link40, link41)
LINK(link39, link40)
LINK(link38, link39)
LINK(link37, link38)
LINK(link36, link37)
LINK(link35, link36)
LINK(link34, link35)
LINK(link33, link34)
LINK(link32, link33)
LINK(link31, link32)
LINK(link30, link31)
LINK(link29, link30)
LINK(link28, link29)
LINK(link27, link28)
LINK(link26, link27)
LINK(link25, link26)
LINK(link24, link25)
LINK(link23, link24)
LINK(link22, link23)
LINK(link21, link22)
LINK(link20, link21)
LINK(link19, link20)
LINK(link18, link19)
LINK(link17, link18)
LINK(link16, link17)
LINK(link15, link16)
LINK(link14, link15)
LINK(link13, link14)
LINK(link12, link13)
LINK(link11, link12)
LINK(link10, link11)
LINK(link9, link10)
LINK(link8, link9)
LINK(link7, link8)
LINK(link6, link7)
LINK(link5, link6)
LINK(link4, link5)
LINK(link3, link4)
LINK(link2, link3)
LINK(link1, link2)
LINK(link0, link1)

void escape_through_long_chain(void)
{
    char buf[10] = {0};
    link0(buf);
}

// at line 94, column 5
// [ !!Warn ] stack pointer escape: address of variable 'buf' escapes this function
//          ↳ address passed as argument to function 'link0' (callee may capture the pointer beyond this function)

// strict-expectation-details: true
