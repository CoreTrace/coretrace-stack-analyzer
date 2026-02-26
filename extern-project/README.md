# extern-project (library consumer example)

This folder demonstrates how to consume `coretrace::stack_usage_analyzer_lib`
from another project and forward analyzer options from your own CLI.

## Build

```bash
cmake -S extern-project -B extern-project/build
cmake --build extern-project/build -j
```

## Run

```bash
./extern-project/build/sa_consumer \
  test/alloca/oversized-constant.c \
  build/compile_commands.json \
  --mode=abi \
  --analysis-profile=fast \
  --warnings-only \
  --jobs=4 \
  --format=sarif
```

Notes:
- Input file is the first positional argument.
- `compile_commands.json` path is the second positional argument.
- All remaining arguments are parsed with the analyzer's CLI parser bridge
  (`ctrace::stack::cli::parseArguments(...)`).
- Do not pass `--compile-commands` / `--compdb` in forwarded args here; use
  the second positional argument.
