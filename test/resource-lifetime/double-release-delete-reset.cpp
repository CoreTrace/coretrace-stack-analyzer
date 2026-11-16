// SPDX-License-Identifier: Apache-2.0
// resource-model: models/resource-lifetime/generic.txt
// Deleting a pointer reset to nullptr deletes nothing: no double release. The function also gets
// a false resource-leak warning ("may be overwritten while still owned"): the null check clang
// emits around `delete` is taken as feasible once the variable is written twice. That warning is
// tracked in #282 and deliberately not pinned here, hence no strict count.
// strict-diagnostic-count: false

void delete_reset_delete()
{
    int* resetp = new int(1);
    delete resetp;
    resetp = nullptr;
    delete resetp;
}

// not contains: potential double release: 'CppHeap' handle 'resetp'
