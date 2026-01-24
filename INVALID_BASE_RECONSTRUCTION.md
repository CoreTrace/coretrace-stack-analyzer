# Invalid Base Reconstruction Detection

## Overview

This feature detects invalid base pointer reconstructions using incorrect `offsetof()` / `container_of()` patterns.

## Problem Detected

Code that reconstructs a "base" pointer from a member pointer (via `container_of` / `offsetof`) can silently produce an invalid pointer if:

- The offset used does not correspond to the actual member
- The calculation goes through integer casts (`ptrtoint`/`inttoptr`) or pointer arithmetic
- The pointer provenance is lost at the IR level

## Detected Patterns

### Pattern 1: inttoptr(ptrtoint(P) +/- C)
```llvm
%ptr_int = ptrtoint ptr %member_ptr to i64
%adjusted = add i64 %ptr_int, <offset>
%reconstructed = inttoptr i64 %adjusted to ptr
```

### Pattern 2: GEP with negative offset
```llvm
%pb = getelementptr %struct.A, ptr %obj, i32 0, i32 1  ; member b
%bad_base = getelementptr i8, ptr %pb, i64 -12         ; incorrect offset!
```

## Examples

### Invalid case (detected as ERROR)

```c
struct A {
    int32_t a;  // offset 0
    int32_t b;  // offset 4
    int32_t c;  // offset 8
    int32_t i;  // offset 12
};

struct A obj;
int32_t *pb = &obj.b;  // pb points to obj + 4

// BUG: uses offsetof(A, i) = 12 instead of offsetof(A, b) = 4
struct A *bad_base = (struct A *)((char *)pb - offsetof(struct A, i));
// Result: bad_base = obj + 4 - 12 = obj - 8  (OUT OF BOUNDS!)
```

**Diagnostic:**
```
at line 19, column 50
[!!] potential UB: invalid base reconstruction via offsetof/container_of
     variable: 'obj'
     source member: offset +4
     offset applied: -12 bytes
     target type: ptr
     [ERROR] derived pointer points OUTSIDE the valid object range
             (this will cause undefined behavior if dereferenced)
```

### Valid case (detected as WARNING)

```c
struct A obj;
int32_t *pb = &obj.b;

// CORRECT: uses offsetof(A, b) = 4
struct A *good_base = (struct A *)((char *)pb - offsetof(struct A, b));
// Result: good_base = obj + 4 - 4 = obj  (OK!)
```

**Diagnostic:**
```
at line 19, column 51
[!!] potential UB: invalid base reconstruction via offsetof/container_of
     variable: 'obj'
     source member: offset +4
     offset applied: -4 bytes
     target type: ptr
     [WARNING] unable to verify that derived pointer points to a valid object
               (potential undefined behavior if offset is incorrect)
```

## Error Code

- **DescriptiveErrorCode::InvalidBaseReconstruction** (value 10)

## Implementation

### Modified Files

1. **include/StackUsageAnalyzer.hpp**
   - Added `InvalidBaseReconstruction` to the `DescriptiveErrorCode` enum
   - Updated `EnumTraits<DescriptiveErrorCode>`

2. **src/StackUsageAnalyzer.cpp**
   - `InvalidBaseReconstructionIssue` structure to store diagnostic information
   - `analyzeInvalidBaseReconstructionsInFunction()` function for detection
   - Integration in `analyzeModule()` to generate diagnostics

### Detection Algorithm

1. **Identify allocas (stack objects)**
   - Calculate the size of each `alloca`
   - Build a map `alloca -> (name, size)`

2. **Detect suspicious patterns**
   - Search for `inttoptr(ptrtoint(P) +/- C)`
   - Search for `getelementptr` with negative offset

3. **Provenance tracking**
   - Follow `bitcast`, `getelementptr`, `load`, `store`
   - Calculate cumulative offset from base `alloca`
   - Identify the source member

4. **Validate resulting pointer**
   - Calculation: `offset_result = offset_member + offset_applied`
   - If `offset_result < 0` or `>= object_size` → **ERROR**
   - Otherwise → **WARNING** (unable to verify completely)

## Test Files

- `test/ct_stack_usage_bad_base.c` : Invalid case (uses wrong offset)
- `test/ct_stack_usage_good_base.c` : Valid case (uses correct offset)

## Compilation and Testing

```bash
# Compilation
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DLLVM_MIN_REQUIRED_VERSION=18
make -j$(nproc)
cd ..

# Generate IR
clang -S -emit-llvm -g -O0 test/ct_stack_usage_bad_base.c -o test/ct_stack_usage_bad_base.ll

# Analysis
./build/stack_usage_analyzer test/ct_stack_usage_bad_base.ll
```

## Limitations

- **Possible false positives**:
  - Code with non-standard structure layout
  - Packed structs
  - Pointer tagging schemes
  
- **Mitigations**:
  - Configuration option to disable this rule
  - Suppression annotations (future work)
  - Whitelist for audited macros

## References

- Original issue: Invalid base pointer reconstruction detection
- `container_of()` pattern used in the Linux kernel
- Potential UB: fabricated pointer used without valid provenance
