; SPDX-License-Identifier: Apache-2.0
; A loop whose two pointer PHIs take each other's value: the cycle p -> q -> p that mem2reg
; leaves in optimized code. Both PHIs must resolve to buf through its decay, and the two stores
; through them must count as stores into buf.

define void @phi_cycle(i32 %n) {
entry:
  %buf = alloca [16 x i8], align 1
  %start = getelementptr inbounds [16 x i8], ptr %buf, i64 0, i64 0
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  %p = phi ptr [ %start, %entry ], [ %q, %loop ]
  %q = phi ptr [ %start, %entry ], [ %p, %loop ]
  %p.slot = getelementptr inbounds i8, ptr %p, i64 20
  store i8 0, ptr %p.slot, align 1
  %q.slot = getelementptr inbounds i8, ptr %q, i64 21
  store i8 1, ptr %q.slot, align 1
  %i.next = add nsw i32 %i, 1
  %more = icmp slt i32 %i.next, %n
  br i1 %more, label %loop, label %exit

exit:
  ret void
}
