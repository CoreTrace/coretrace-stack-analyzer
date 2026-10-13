// SPDX-License-Identifier: Apache-2.0
// #217: types written by macros. A macro argument is rewritten where it is written; a macro that
// writes only the parameter gets const before its invocation, since a macro is text. A macro
// definition that writes the declaration is not rewritten: the suggestion is kept, not checked.

// strict-expectation-details: true
// not contains: parameter 'buffer' in function 'object_macro_callback'
// not contains: ↳ ABI:
#define BYTE unsigned char
#define BYTE_POINTER unsigned char*
#define BUFFER_PARAM(name) unsigned char* name
#define DECLARE_READER(name, type) int name(type buffer)
#define READER_DECLARATIONS int written_by_definition(unsigned char* buffer);

typedef unsigned char* byte_pointer;
#define BYTE_TYPEDEF byte_pointer

// at line 23, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'buffer' in function 'object_macro' is never used to modify the pointed object
// ↳ current type: unsigned char *buffer
// ↳ suggested type: const unsigned char *buffer
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches once debug information, metadata numbering and renamed symbols are normalized
int object_macro(BYTE* buffer)
{
    return buffer[0];
}

// at line 33, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'buffer' in function 'pointer_macro' is never used to modify the pointed object
// ↳ current type: unsigned char *buffer
// ↳ suggested type: const unsigned char *buffer
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches once debug information, metadata numbering and renamed symbols are normalized
int pointer_macro(BYTE_POINTER buffer)
{
    return buffer[0];
}

// at line 43, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'buffer' in function 'declarator_macro' is never used to modify the pointed object
// ↳ current type: unsigned char *buffer
// ↳ suggested type: const unsigned char *buffer
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches once debug information, metadata numbering and renamed symbols are normalized
int declarator_macro(BUFFER_PARAM(buffer))
{
    return buffer[0];
}

// at line 53, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'buffer' in function 'argument_macro' is never used to modify the pointed object
// ↳ current type: unsigned char *buffer
// ↳ suggested type: const unsigned char *buffer
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches once debug information, metadata numbering and renamed symbols are normalized
DECLARE_READER(argument_macro, unsigned char*)
{
    return buffer[0];
}

// at line 63, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'buffer' in function 'typedef_macro' is never used to modify the pointed object
// ↳ current type: unsigned char *buffer
// ↳ suggested type: const unsigned char *buffer
// ↳ not checked with Clang: the parameter's typedef is written by a macro
int typedef_macro(BYTE_TYPEDEF buffer)
{
    return buffer[0];
}

READER_DECLARATIONS

// at line 75, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'buffer' in function 'written_by_definition' is never used to modify the pointed object
// ↳ current type: unsigned char *buffer
// ↳ suggested type: const unsigned char *buffer
// ↳ not checked with Clang: the declaration is written by a macro definition
int written_by_definition(unsigned char* buffer)
{
    return buffer[0];
}

// A macro type, and a callback of the old type: the const version does not compile.
int object_macro_callback(BYTE* buffer)
{
    return buffer[0];
}

int apply(int (*reader)(BYTE*), BYTE* buffer);

int run_callback(BYTE* buffer)
{
    return apply(object_macro_callback, buffer);
}
