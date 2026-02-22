// resource-model: models/resource-lifetime/generic.txt
int new_balanced_delete()
{
    int* p = new int(1);
    delete p;
    return 0;
}

// not contains: potential resource leak: 'CppHeap' acquired in handle 'p' is not released in this function
// not contains: potential double release: 'CppHeap' handle 'p' is released without a matching acquire in this function
