#include <stdint.h>

// at line 13, column 0
// [!]ConstParameterNotModified.Pointer: parameter 'param3' in function 'myfunc' is never used to modify the pointed object
//     current type: int32_t *param3
//     suggested type: const int32_t *param3

// at line 13, column 0
// [!]ConstParameterNotModified.PointerConstOnly: parameter 'param4' in function 'myfunc' is declared 'int32_t * const param4' but the pointed object is never modified
//     consider 'const int32_t *param4' for API const-correctness
//     current type: int32_t * const param4
//     suggested type: const int32_t *param4
void myfunc(int32_t* out, const int32_t* in, int32_t* param3, int32_t* const param4)
{
    *out = *in + *param3 + *param4;
}

void set_zero(int* p)
{
    *p = 0;
}
