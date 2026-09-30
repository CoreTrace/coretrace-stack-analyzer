// SPDX-License-Identifier: Apache-2.0
//
// A second definition of peek() that writes through p (#157). A symbol defined by several files
// never proves "writes nothing", least of all when one definition writes.

void peek(int* p)
{
    *p = 1;
}
