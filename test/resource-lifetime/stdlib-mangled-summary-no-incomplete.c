// SPDX-License-Identifier: Apache-2.0
typedef void* GenericHandle;

extern void release_handle(GenericHandle);

void fake_stdlib_mangled_destructor(GenericHandle handle) __asm__(
    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEED2Ev");
void fake_stdlib_mangled_destructor(GenericHandle handle)
{
    release_handle(handle);
}

// resource-model: models/resource-lifetime/generic.txt
int resource_lifetime_stdlib_mangled_summary_no_incomplete(void)
{
    GenericHandle h = (GenericHandle)0;
    fake_stdlib_mangled_destructor(h);
    return 0;
}

// not contains: inter-procedural resource analysis incomplete: handle 'h'
