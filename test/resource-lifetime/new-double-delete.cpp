// resource-model: models/resource-lifetime/generic.txt
int new_double_delete()
{
    int* p = new int(3);
    delete p;
    delete p;
    return 0;
}

// at line 6, column 5
// [!!!Error] potential double release: 'CppHeap' handle 'p' is released without a matching acquire in this function
