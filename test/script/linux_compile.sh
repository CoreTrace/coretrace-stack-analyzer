#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
CC_BIN="${CC_BIN:-$BUILD_DIR/cc}"

if [ ! -x "$CC_BIN" ]; then
  echo "cc binary not found at $CC_BIN"
  echo "Build it first (see README.md)."
  exit 1
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

cat >"$TMP_DIR/foo.cpp" <<'EOF'
#include <string>
int foo() {
    std::string s = "foo";
    return static_cast<int>(s.size());
}
EOF

cat >"$TMP_DIR/bar.cpp" <<'EOF'
#include <vector>
extern int foo();
int bar() {
    std::vector<int> v{1, 2, 3};
    return static_cast<int>(v.size());
}
int main() {
    return (foo() + bar() == 6) ? 0 : 1;
}
EOF

cat >"$TMP_DIR/baz.c" <<'EOF'
#include <string>
int baz() {
    std::string s = "baz";
    return static_cast<int>(s.size());
}
EOF

pushd "$TMP_DIR" >/dev/null

"$CC_BIN" --instrument foo.cpp bar.cpp -o app_instrumented
./app_instrumented

"$CC_BIN" --instrument -x c++ -c foo.cpp bar.cpp
"$CC_BIN" --instrument -x=c++ -c baz.c -o=baz_inst.o
"$CC_BIN" --instrument foo.o bar.o -o app_instrumented_obj
./app_instrumented_obj
"$CC_BIN" --instrument foo.o bar.o baz_inst.o -o=app_instrumented_obj_eq
./app_instrumented_obj_eq

rm -f foo.o bar.o baz_inst.o

"$CC_BIN" foo.cpp bar.cpp -o app_plain
./app_plain

"$CC_BIN" -x c++ -c foo.cpp bar.cpp
"$CC_BIN" foo.o bar.o -o app_plain_obj
./app_plain_obj
"$CC_BIN" -x=c++ -c baz.c -o=baz_eq.o
"$CC_BIN" foo.o bar.o baz_eq.o -o=app_plain_obj_eq
./app_plain_obj_eq

rm -f foo.o bar.o baz_eq.o

popd >/dev/null
