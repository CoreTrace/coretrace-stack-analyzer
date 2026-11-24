// SPDX-License-Identifier: Apache-2.0
// #217: a parameter is reported only if the analyzed files still compile with its suggested type
// and generate the same code. Here the const version does not compile, or selects other code.

// strict-expectation-details: true
// not contains: parameter 'c' in function 'writer'
// not contains: parameter 'slot' in function 'read_slot'
// not contains: parameter 'p' in function 'address_to_int_pp'
// not contains: parameter 'p' in function 'generic_selection'
// not contains: ↳ ABI:

struct ctx
{
    int* value;
};

void apply(void (*callback)(struct ctx*), struct ctx* c);

// Passed where a callback with a non-const parameter is expected.
static void writer(struct ctx* c)
{
    (void)c->value;
}

void run(struct ctx* c)
{
    apply(writer, c);
}

// Its address initializes a pointer whose type has a non-const parameter.
static int read_slot(int* slot)
{
    return *slot;
}

int (*const slot_reader)(int*) = read_slot;

// &p is passed where int** is expected: const would differ in a nested qualifier.
static int read_pp(int** pp)
{
    return **pp;
}

int address_to_int_pp(int* p)
{
    return read_pp(&p);
}

// _Generic selects on the parameter's type: the const version compiles, but returns 0 instead
// of reading *p.
#define READ_ONLY(x) _Generic((x), const int*: 1, default: 0)

int generic_selection(int* p)
{
    if (READ_ONLY(p))
        return 0;
    return *p;
}

// Control: nothing depends on the parameter's type.
// at line 66, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'read_value' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
int read_value(int* p)
{
    return *p;
}

// The Clang check of const suggestions is experimental: it runs only with this option.
// analyzer-arg: --experimental-const-suggestion-check
