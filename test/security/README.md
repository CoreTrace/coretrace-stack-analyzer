# Security Fixture Corpus

This corpus groups the numbered security fixtures by vulnerability type.

## Layout by type

```
buffer-overflow/
  01_buffer_overflow.c

command-injection/
  08_command_injection.c

format-string/
  02_format_string.c

integer-overflow/
  04_integer_overflow.c
  17_integer_overflow_advanced.c

memory-leak/
  09_memory_leak.c

null-dereference/
  05_null_deref.c
  16_null_deref_nested.c

oob-read/
  12_oob_read.c

sizeof-pitfall/
  15_sizeof_pitfall.c

stack-escape/
  11_return_local.c

toctou/
  07_toctou.c

type-confusion/
  10_type_confusion.c

uninitialized/
  06_uninitialized.c

unsafe-functions/
  13_unsafe_functions.c

use-after-free/
  03_use_after_free.c
  18_use_after_free_advanced.c

variadic-mismatch/
  14_variadic_mismatch.c
```

## Quick usage

```bash
# from repository root
python3 run_test.py

# from this directory
make all
make analyze
make asan
make clean
```

## Notes

- `run_test.py` now applies strict warning/error expectation count checks by
  default across all fixture files under `test/` (this corpus included).
- Legacy `test/files` remains as a compatibility shim.
