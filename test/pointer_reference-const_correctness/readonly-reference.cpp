// at line 5, column 0
// [!]ConstParameterNotModified.Reference: parameter 'inc' in function 'increment(int&, int&)' is never used to modify the referred object
//     current type: int &inc
//     suggested type: const int &inc
void increment(int& value, int& inc)
{
    value += inc;
}

// at line 14, column 0
// [!]ConstParameterNotModified.ReferenceRvaluePreferValue: parameter 'value' in function 'read_once(int&&)' is an rvalue reference and is never used to modify the referred object
//     consider passing by value (int value) or const reference (const int &value)
//     current type: int &&value
int read_once(int&& value)
{
    return value;
}
