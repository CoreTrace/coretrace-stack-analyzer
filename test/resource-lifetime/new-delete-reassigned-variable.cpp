// SPDX-License-Identifier: Apache-2.0
// resource-model: models/resource-lifetime/generic.txt
// #282: clang tests the pointer against null before `delete p`. When `p` holds the result of a
// throwing new, the null edge is never taken, also when the variable is written again, as in
// `delete p; p = nullptr;`. A leak after an overwrite stays reported.

void delete_once()
{
    int* once = new int(1);
    delete once;
}

void delete_then_reset()
{
    int* reset = new int(1);
    delete reset;
    reset = nullptr;
}

void delete_reset_delete()
{
    int* again = new int(1);
    delete again;
    again = nullptr;
    delete again;
}

void delete_then_reuse_slot()
{
    int* slot = new int(1);
    delete slot;
    slot = new int(2);
    delete slot;
}

void delete_copy_then_reset()
{
    int* owner = new int(1);
    int* alias = owner;
    delete alias;
    alias = nullptr;
}

void user_check_then_reset()
{
    int* checked = new int(1);
    if (checked == nullptr)
        return;
    delete checked;
    checked = nullptr;
}

void overwrite_without_delete()
{
    int* lost = new int(1);
    lost = new int(2);
    delete lost;
}

void delete_on_one_branch(bool release)
{
    int* maybe = new int(1);
    if (release)
        delete maybe;
    maybe = new int(2);
    delete maybe;
}

void reuse_without_second_delete()
{
    int* reused = new int(1);
    delete reused;
    reused = new int(2);
}

void reset_without_delete()
{
    int* dropped = new int(1);
    dropped = nullptr;
}

void reset_then_delete()
{
    int* late = new int(1);
    late = nullptr;
    delete late;
}

// not contains: potential resource leak: 'CppHeap' acquired in handle 'once'
// not contains: potential resource leak: 'CppHeap' acquired in handle 'reset'
// not contains: potential resource leak: 'CppHeap' acquired in handle 'again'
// not contains: potential resource leak: 'CppHeap' acquired in handle 'slot'
// not contains: potential resource leak: 'CppHeap' acquired in handle 'owner'
// not contains: potential resource leak: 'CppHeap' acquired in handle 'checked'
// not contains: potential resource leak: 'CppHeap' acquired in handle 'lost' may leave the function without being released
// not contains: potential resource leak: 'CppHeap' acquired in handle 'maybe' may leave the function without being released
// not contains: potential resource leak: 'CppHeap' acquired in handle 'reused' may be overwritten while still owned

// at line 55, column 17
// [ !!Warn ] potential resource leak: 'CppHeap' acquired in handle 'lost' is overwritten while still owned

// at line 62, column 18
// [ !!Warn ] potential resource leak: 'CppHeap' acquired in handle 'maybe' may be overwritten while still owned

// at line 73, column 14
// [ !!Warn ] potential resource leak: 'CppHeap' acquired in handle 'reused' is not released in this function

// at line 78, column 20
// [ !!Warn ] potential resource leak: 'CppHeap' acquired in handle 'dropped' is overwritten while still owned

// at line 84, column 17
// [ !!Warn ] potential resource leak: 'CppHeap' acquired in handle 'late' is overwritten while still owned
