// at line 5, column 0
// [!]ConstParameterNotModified.Reference: parameter 'inc' in function 'increment(int&, int&)' is never used to modify the referred object
//     current type: int &inc
//     suggested type: const int &inc
void increment(int &value, int &inc)
{
    value += inc;
}

int read_once(int &&value)
{
    // at line 10, column 0
    // [!]ConstParameterNotModified.ReferenceRvalue: parameter 'value' in function 'read_once(int&&)' is never used to modify the referred object
    //     current type: int &&value
    //     suggested type: const int &&value
    return value;
}
