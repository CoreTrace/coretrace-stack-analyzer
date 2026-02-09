#include <cstdint>
#include <cstring>
#include <iostream>

int main(int argc, char** argv)
{
    std::string value;
    std::uint16_t value16 = 0;

    for (int i = 1; i < argc; ++i)
    {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0)
        {
            return 1;
        }
    }
    std::cout << value << std::endl;
    return 0;
}

// not contains: potential read of uninitialized local variable 'argc.addr'
// not contains: potential read of uninitialized local variable 'argv.addr'
