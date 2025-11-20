# coretrace-stack-analyzer

#### BUILD

```zsh
./build.sh
```

#### CORETRACE-STACK-USAGE CLI

```zsh
./stack_usage_analyzer --mode=[abi/ir] test.[ll/c/cpp]
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

##### TODO:
- Library mode
- Define json API
- Unmangling symbols

