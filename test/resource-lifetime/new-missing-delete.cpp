// SPDX-License-Identifier: Apache-2.0
// windows-skip: C++ heap lifetime diagnostics for missing delete are not yet emitted reliably on Windows.
// resource-model: models/resource-lifetime/generic.txt
int new_missing_delete()
{
    int* p = new int(2);
    return *p;
}

// at line 4, column 14
// [ !!Warn ] potential resource leak: 'CppHeap' acquired in handle 'p' is not released in this function
