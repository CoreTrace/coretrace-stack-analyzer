# coretrace-stack-analyzer

#### BUILD

```zsh
./build.sh
```

#### CORETRACE-STACK-USAGE CLI

```zsh
./stack_usage_analyzer --mode=[abi/ir] test.[ll/c/cpp]
```

```
--quiet coupe complètement les diagnostics
--warnings-only garde seulement les diagnostics importants
````

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

#### 9. Détection de fuite de stack pointer

Exemples :
```c
char buf[10];
return buf;    // renvoi pointeur vers stack → use-after-return
```

Ou stockage :

```c
global = buf; // leaking address of stack variable
```

---

Actually done:

- 1. Dynamic alloca / VLA detection, including user-controlled sizes, upper-bound inference, and recursion-aware severity (errors for infinite recursion or oversized allocations, warnings for other dynamic sizes).
- 2. Deriving human-friendly names for unnamed allocas in diagnostics.
- 3. Detection of memcpy/memset overflows on stack buffers.
- 4. Warning when a function performs multiple stores into the same stack buffer.
- 5. Deeper traversal analysis: constraint propagation.
- 6. Detection of deep indirection in aliasing.
- 7. Detection of overflow in a struct containing an internal array.
- 8. Detection of stack pointer leaks:
	- store_unknown -> storing the pointer in a non-local location (typically out-parameter, heap, etc.)
	- call_callback -> passing it to a callback (indirect call)
	- call_arg -> passing it as an argument to a direct function, potentially capturable
