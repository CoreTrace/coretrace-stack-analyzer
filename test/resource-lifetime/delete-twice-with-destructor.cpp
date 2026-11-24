// SPDX-License-Identifier: Apache-2.0
// resource-model: models/resource-lifetime/generic.txt
// Unlike new-double-delete.cpp, the second delete runs a destructor on the released object: a
// use after release as well as a double release.
struct Obj
{
    int* data;
    ~Obj();
};

void delete_twice()
{
    Obj* o = new Obj();
    delete o;
    delete o;
}

// at line 15, column 5
// [!!!Error] potential double release: 'CppHeap' handle 'o' is released without a matching acquire in this function

// at line 15, column 12
// [!!!Error] potential use-after-release: 'CppHeap' handle 'o' is used after a release in this function
