// SPDX-License-Identifier: Apache-2.0
extern "C" char* __cxa_demangle(const char* mangled, char* outputBuffer, unsigned long* length,
                                int* status);
extern "C" void free(void*);

// resource-model: models/resource-lifetime/generic.txt
int resource_lifetime_cxa_demangle_balanced_no_incomplete(const char* symbol)
{
    int status = 0;
    char* demangled = __cxa_demangle(symbol, nullptr, nullptr, &status);
    free(demangled);
    return status;
}

// not contains: inter-procedural resource analysis incomplete
// not contains: potential double release
