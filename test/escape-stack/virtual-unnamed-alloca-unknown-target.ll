; Regression: an unnamed alloca (origin unknown) used as callback arg in a
; virtual-like indirect call must still trigger stack escape warning.
; This prevents suppressing real escapes just because the alloca has no name.

define void @test_virtual_unknown(ptr %obj) {
entry:
  %1 = alloca i32, align 4
  %2 = load ptr, ptr %obj, align 8
  %3 = getelementptr inbounds ptr, ptr %2, i64 0
  %4 = load ptr, ptr %3, align 8
  call void %4(ptr %1)
  ret void
}
