// SPDX-License-Identifier: Apache-2.0
// #217: the suggestion is kept when the generated code is the same. log_text's join block has
// three predecessors, which the printed IR lists in a comment, in an order that copying a module
// may change: comments are not code.

// strict-expectation-details: true
#include <string>

void sink(const std::string& text);

static void log_text(const std::string& text)
{
    if (text.empty())
        return;
    if (text.back() == '\n')
    {
        sink(text);
    }
    else
    {
        sink(text + "\n");
    }
}

// at line 31, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'read_and_log(int*)' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
// ↳ ABI: the suggested type changes the symbol (_Z12read_and_logPi -> _Z12read_and_logPKi): code compiled separately must use the updated declaration and be recompiled
int read_and_log(int* p)
{
    log_text("value");
    return *p;
}

// The Clang check of const suggestions is experimental: it runs only with this option.
// analyzer-arg: --experimental-const-suggestion-check
