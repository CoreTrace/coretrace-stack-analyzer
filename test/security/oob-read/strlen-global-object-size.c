// SPDX-License-Identifier: Apache-2.0
//
// Pins the object sizes OOBReadAnalysis obtains from FunctionFacts::objectSizeBytes(), which
// is backed by ObjectSizeOffsetVisitor rather than the hand-rolled alloca/global sizing it
// replaced. Both buffers are filled to their full width with no room for a terminator, so
// the strlen that follows must be reported for each of them; that only happens if the
// visitor returns exactly 16 bytes for both a defining and a declaring global.
#include <string.h>

extern char declared_buf[16];
static char defined_buf[16];

size_t scan_declared(void)
{
    memcpy(declared_buf, "AAAAAAAAAAAAAAAA", 16);
    return strlen(declared_buf);
}

size_t scan_defined(void)
{
    memcpy(defined_buf, "AAAAAAAAAAAAAAAA", 16);
    return strlen(defined_buf);
}

// strict-expectation-details: true

// at line 16, column 12
// [ !!Warn ] potential out-of-bounds read: string buffer 'declared_buf' may be missing a null terminator before 'strlen'
// ↳ buffer size: 16 bytes, last write size: 16 bytes
// ↳ unterminated strings can make read APIs scan past buffer bounds

// at line 22, column 12
// [ !!Warn ] potential out-of-bounds read: string buffer 'defined_buf' may be missing a null terminator before 'strlen'
// ↳ buffer size: 16 bytes, last write size: 16 bytes
// ↳ unterminated strings can make read APIs scan past buffer bounds
