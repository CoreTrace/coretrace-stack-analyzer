#!/bin/bash
# Test script for invalid base reconstruction detection

set -e

echo "========================================="
echo "Invalid Base Reconstruction - Test Suite"
echo "========================================="
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test counter
PASSED=0
FAILED=0

# Function to run a test
run_test() {
    local name=$1
    local file=$2
    local opt=$3
    local expected=$4
    
    echo -n "Testing $name... "
    
    # Generate IR
    clang -S -emit-llvm -g $opt "$file.c" -o "$file.ll" 2>/dev/null
    
    # Run analyzer
    output=$(./build/stack_usage_analyzer "$file.ll" 2>&1)
    
    # Check for expected result
    if echo "$output" | grep -q "$expected"; then
        echo -e "${GREEN}PASS${NC}"
        PASSED=$((PASSED + 1))
    else
        echo -e "${RED}FAIL${NC}"
        echo "Expected: $expected"
        echo "Got: $output"
        FAILED=$((FAILED + 1))
    fi
}

# Run tests
run_test "Bad Base (ERROR)" "test/ct_stack_usage_bad_base" "-O0" "\\[ERROR\\] derived pointer points OUTSIDE"
run_test "Good Base (WARNING)" "test/ct_stack_usage_good_base" "-O0" "\\[WARNING\\] unable to verify"
run_test "Container_of Invalid" "test/test_container_of" "-O0" "\\[ERROR\\] derived pointer points OUTSIDE"
run_test "PtrToInt Pattern (O1)" "test/test_ptrtoint_pattern" "-O1" "\\[ERROR\\] derived pointer points OUTSIDE"

echo ""
echo "========================================="
printf "Test Results: ${GREEN}%d passed${NC}, ${RED}%d failed${NC}\n" "$PASSED" "$FAILED"
echo "========================================="

if [ $FAILED -eq 0 ]; then
    exit 0
else
    exit 1
fi
