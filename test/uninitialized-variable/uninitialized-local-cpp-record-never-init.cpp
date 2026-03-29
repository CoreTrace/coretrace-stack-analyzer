// SPDX-License-Identifier: Apache-2.0
struct NonEmptyRecord
{
    int value;

    int get() const
    {
        return value;
    }
};

int non_empty_record_method_read_should_warn(void)
{
    NonEmptyRecord obj;
    return obj.get();
}

// at line 14, column 16
// [ !!Warn ] potential read of uninitialized local variable 'obj'
// ↳ this call may read the value before any definite initialization in '_ZNK14NonEmptyRecord3getEv'
