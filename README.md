# coretrace-stack-analyzer

#### BUILD

```zsh
./build.sh
```

### Code style (clang-format)

- Target version: `clang-format` 20 (used in CI).
- Format locally: `./scripts/format.sh`
- Check without modifying: `./scripts/format-check.sh`
- CMake: `cmake --build build --target format` or `--target format-check`
- CI: the GitHub Actions `clang-format` job fails if a file is not formatted.

#### CORETRACE-STACK-USAGE CLI

```zsh
./stack_usage_analyzer --mode=[abi/ir] test.[ll/c/cpp] other.[ll/c/cpp]
./stack_usage_analyzer main.cpp -I./include
./stack_usage_analyzer main.cpp -I./include --compile-arg=-I/opt/homebrew/opt/llvm@20/include
./stack_usage_analyzer main.cpp -I./include --only-file=./main.cpp --only-function=main
```

```
--format=json|sarif|human
--quiet disables diagnostics entirely
--warnings-only keeps only important diagnostics
--stack-limit=<value> overrides stack limit (bytes, or KiB/MiB/GiB)
--compile-arg=<arg> passes an extra argument to the compiler
-I<dir> or -I <dir> adds an include directory
-D<name>[=value] or -D <name>[=value] defines a macro
--only-file=<path> or --only-file <path> filters by file
--only-dir=<path> or --only-dir <path> filters by directory
--only-function=<name> or --only-function <name> filters by function
--only-func=<name> alias for --only-function
--dump-filter prints filter decisions (stderr)
```

### Example

Given this code:

```C
#define SIZE_LARGE 8192000000
#define SIZE_SMALL (SIZE_LARGE / 2)

int main(void)
{
    char test[SIZE_LARGE];

    return 0;
}
```

You can pass either the `.c` file or the corresponding `.ll` file to the analyzer.
You may receive the following output:

```zsh
Language: C
Compiling source file to LLVM IR...
Mode: ABI

Function: main
  local stack: 4096000016 bytes
  max stack (including callees): 4096000016 bytes
  [!] potential stack overflow: exceeds limit of 8388608 bytes
```

---

Given this code:

```C
int foo(void)
{
    char test[8192000000];
    return 0;
}

int bar(void)
{
    return 0;
}

int main(void)
{
    foo();
    bar();

    return 0;
}
```

Depending on the selected `--mode`, you may obtain the following results:

```zsh
Language: C
Compiling source file to LLVM IR...
Mode: ABI

Function: foo
  local stack: 8192000000 bytes
  max stack (including callees): 8192000000 bytes
  [!] potential stack overflow: exceeds limit of 8388608 bytes

Function: bar
  local stack: 16 bytes
  max stack (including callees): 16 bytes

Function: main
  local stack: 32 bytes
  max stack (including callees): 8192000032 bytes
  [!] potential stack overflow: exceeds limit of 8388608 bytes
```

```zsh
Language: C
Compiling source file to LLVM IR...
Mode: IR

Function: foo
  local stack: 8192000000 bytes
  max stack (including callees): 8192000000 bytes
  [!] potential stack overflow: exceeds limit of 8388608 bytes

Function: bar
  local stack: 0 bytes
  max stack (including callees): 0 bytes

Function: main
  local stack: 16 bytes
  max stack (including callees): 8192000016 bytes
  [!] potential stack overflow: exceeds limit of 8388608 bytes
```

---

#### 9. Stack pointer leak detection

Examples:
```c
char buf[10];
return buf;    // renvoi pointeur vers stack → use-after-return
```

Or storing:

```c
global = buf; // leaking address of stack variable
```

---

Actually done:

- 1. Multi-file CLI inputs with deterministic ordering and aggregated output.
- 2. Per-result file attribution in JSON/SARIF and diagnostics.
- 3. Filters: `--only-file`, `--only-dir`, `--only-function/--only-func`, plus `--dump-filter`.
- 4. Compile args passthrough: `-I`, `-D`, `--compile-arg`.
- 5. Dynamic alloca / VLA detection, including user-controlled sizes, upper-bound inference, and recursion-aware severity (errors for infinite recursion or oversized allocations, warnings for other dynamic sizes).
- 6. Deriving human-friendly names for unnamed allocas in diagnostics.
- 7. Detection of memcpy/memset overflows on stack buffers.
- 8. Warning when a function performs multiple stores into the same stack buffer.
- 9. Deeper traversal analysis: constraint propagation.
- 10. Detection of deep indirection in aliasing.
- 11. Detection of overflow in a struct containing an internal array.
- 12. Detection of stack pointer leaks:
	- store_unknown -> storing the pointer in a non-local location (typically out-parameter, heap, etc.)
	- call_callback -> passing it to a callback (indirect call)
	- call_arg -> passing it as an argument to a direct function, potentially capturable
