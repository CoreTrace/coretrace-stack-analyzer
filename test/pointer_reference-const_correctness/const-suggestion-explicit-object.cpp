// SPDX-License-Identifier: Apache-2.0
// #217: an explicit object parameter takes the first place in the parameters' debug numbering, as
// an implicit `this` would not: the check must rewrite p, not self. read's address is stored with
// the old type, so read's suggested type does not compile and is removed; peek's is checked.

// analyzer-arg: --experimental-const-suggestion-check
// analyzer-arg: --compile-arg=-std=c++23
// not contains: in function 'Reader::read(
// strict-expectation-details: true

// at line 22, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'Reader::peek(this Reader const&, int*)' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
struct Reader
{
    int read(this const Reader& self, int* p)
    {
        return *p;
    }
    int peek(this const Reader& self, int* p)
    {
        return *p;
    }
};

int (*stored)(const Reader&, int*) = &Reader::read;

int use_reader()
{
    Reader reader;
    int value = 1;
    return reader.read(&value) == reader.peek(&value);
}
