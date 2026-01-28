#include <utility>

void consume_ref(const int &v) {
    (void)v;
}

void consume_ref_mut(int &v) {
    v += 1;
}

// at line 15, column 0
// [!]ConstParameterNotModified.Reference: parameter 'v' in function 'caller_ref(int&)' is never used to modify the referred object
//     current type: int &v
//     suggested type: const int &v
void caller_ref(int &v) {
    consume_ref(v);
}

void caller_ref_mut(int &v) {
    consume_ref_mut(v);
}

// at line 27, column 0
// [!]ConstParameterNotModified.ReferenceRvalue: parameter 'v' in function 'rvalue_use(int&&)' is never used to modify the referred object
//     current type: int &&v
//     suggested type: const int &&v
int rvalue_use(int &&v) {
    return v;
}

// at line 35, column 0
// [!]ConstParameterNotModified.Pointer: parameter 'p' in function 'read_only_ptr(int*)' is never used to modify the pointed object
//     current type: int *p
//     suggested type: const int *p
void read_only_ptr(int *p) {
    int x = *p;
    (void)x;
}
