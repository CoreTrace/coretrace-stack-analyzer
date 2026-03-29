// SPDX-License-Identifier: Apache-2.0
// at line 5, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'src' in function 'deep_alias' is never used to modify the pointed object
//          ↳ current type: char *src
//          ↳ suggested type: const char *src
void deep_alias(char* src)
{
    char buf[10];
    char* p1 = buf;
    char* p2 = p1;
    char** pp = &p2;

    // at line 19, column 18
    // [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 10)
    //          ↳ alias path: buf -> arraydecay -> p1 -> p2 -> pp
    //          ↳ index variable may go up to 19 (array last valid index: 9)
    //          ↳ (this is a write access)
    for (int i = 0; i < 20; ++i)
    {
        (*pp)[i] = src[i];
    }
}

int main(void)
{
    char src[20] = {0};
    deep_alias(src);
    return 0;
}

// at line 7, column 1
// [ !!Warn ] local variable 'buf' is never initialized
//          ↳ declared without initializer and no definite write was found in this function