// SPDX-License-Identifier: Apache-2.0
// resource-model: models/resource-lifetime/generic.txt
// Deleting a pointer reset to nullptr deletes nothing: no double release, and no leak (#282).

void delete_reset_delete()
{
    int* resetp = new int(1);
    delete resetp;
    resetp = nullptr;
    delete resetp;
}

// not contains: potential double release: 'CppHeap' handle 'resetp'
// not contains: potential resource leak: 'CppHeap' acquired in handle 'resetp'
