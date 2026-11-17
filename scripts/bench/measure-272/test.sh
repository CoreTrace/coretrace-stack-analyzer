#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Runs the tests of /work/<variant> as the CI does on Linux (ci.yml), from empty caches.
#   test.sh <main|rework>
set -euo pipefail

cd "/work/$1"
# run_test.py skips the unit tests when their binaries are missing: require them here.
for bin in stack_usage_analyzer_unit_tests ownership_engine_unit_tests; do
    test -x "build/${bin}" || {
        echo "build/${bin} is missing" >&2
        exit 1
    }
done
rm -rf .cache
CORETRACE_RUN_TEST_EXTRA_ANALYZER_ARGS="--compile-ir-cache-dir=.cache/compile-ir" \
    python3 -u run_test.py --jobs=4 --analyzer-timeout=120
