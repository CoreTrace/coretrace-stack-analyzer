// SPDX-License-Identifier: Apache-2.0
struct BadCtor
{
    int initialized;
    int forgotten;
    BadCtor(int v) : initialized(v) {} // does not initialize 'forgotten'
};

int ctor_forgets_field_should_warn(void)
{
    BadCtor obj(42);
    return obj.forgotten;
}

// at line 11, column 16
// [ !!Warn ] potential read of uninitialized local variable 'obj'
//          ↳ this load may execute before any definite initialization on all control-flow paths
