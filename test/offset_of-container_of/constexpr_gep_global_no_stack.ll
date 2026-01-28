; Test: ConstantExpr GEP in ptrtoint operand (global, not stack)

%struct.A = type { i32, i32, i32, i32 }

@G = global %struct.A zeroinitializer, align 4

define i32 @test_constexpr() {
entry:
  %ptrint = ptrtoint ptr getelementptr (%struct.A, ptr @G, i64 0, i32 1) to i64
  %addr = add i64 %ptrint, -12
  %base = inttoptr i64 %addr to ptr
  %val = load i32, ptr %base, align 4
  ret i32 %val
}
