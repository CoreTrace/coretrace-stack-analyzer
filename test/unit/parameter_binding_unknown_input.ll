; SPDX-License-Identifier: Apache-2.0
; A parameter's declaration gives its type; when it gives none, the debug type of the function
; serves only where its entries are the declared parameters in order: when no artificial parameter
; other than `this` exists. vtt, is_most_derived or a block's descriptor make the position
; ambiguous, since the debug type lists some of them and not others.

; An artificial parameter other than `this`: the debug type's entries are ambiguous. x's
; declaration gives its type, double *, which is kept.
define void @ambiguous_typed(ptr %hidden, ptr %x) !dbg !20 {
entry:
  %hidden.addr = alloca ptr
  %x.addr = alloca ptr
  store ptr %hidden, ptr %hidden.addr
    #dbg_declare(ptr %hidden.addr, !21, !DIExpression(), !23)
  store ptr %x, ptr %x.addr
    #dbg_declare(ptr %x.addr, !22, !DIExpression(), !23)
  ret void, !dbg !23
}

; The same, but x's declaration gives no type: x stays without one.
define void @ambiguous_untyped(ptr %hidden, ptr %x) !dbg !30 {
entry:
  %hidden.addr = alloca ptr
  %x.addr = alloca ptr
  store ptr %hidden, ptr %hidden.addr
    #dbg_declare(ptr %hidden.addr, !31, !DIExpression(), !33)
  store ptr %x, ptr %x.addr
    #dbg_declare(ptr %x.addr, !32, !DIExpression(), !33)
  ret void, !dbg !33
}

; No artificial parameter: the debug type's entries are the parameters, in order. x's declaration
; gives no type, the debug type gives double *.
define void @established_untyped(ptr %x) !dbg !40 {
entry:
  %x.addr = alloca ptr
  store ptr %x, ptr %x.addr
    #dbg_declare(ptr %x.addr, !41, !DIExpression(), !42)
  ret void, !dbg !42
}

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3}

!0 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus_14, file: !1, producer: "hand-written", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "parameter_binding_unknown_input.ll", directory: ".")
!2 = !{i32 7, !"Dwarf Version", i32 5}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!10 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!11 = !DIBasicType(name: "double", size: 64, encoding: DW_ATE_float)
!12 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !10, size: 64)
!13 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !11, size: 64)
!14 = !DISubroutineType(types: !15)
!15 = !{null, !12, !12, !12}
!16 = !DISubroutineType(types: !17)
!17 = !{null, !13}
!20 = distinct !DISubprogram(name: "ambiguous_typed", scope: !1, file: !1, line: 1, type: !14, scopeLine: 1, spFlags: DISPFlagDefinition, unit: !0)
!21 = !DILocalVariable(name: "hidden", arg: 1, scope: !20, file: !1, line: 1, type: !12, flags: DIFlagArtificial)
!22 = !DILocalVariable(name: "x", arg: 2, scope: !20, file: !1, line: 1, type: !13)
!23 = !DILocation(line: 1, scope: !20)
!30 = distinct !DISubprogram(name: "ambiguous_untyped", scope: !1, file: !1, line: 2, type: !14, scopeLine: 2, spFlags: DISPFlagDefinition, unit: !0)
!31 = !DILocalVariable(name: "hidden", arg: 1, scope: !30, file: !1, line: 2, type: !12, flags: DIFlagArtificial)
!32 = !DILocalVariable(name: "x", arg: 2, scope: !30, file: !1, line: 2)
!33 = !DILocation(line: 2, scope: !30)
!40 = distinct !DISubprogram(name: "established_untyped", scope: !1, file: !1, line: 3, type: !16, scopeLine: 3, spFlags: DISPFlagDefinition, unit: !0)
!41 = !DILocalVariable(name: "x", arg: 1, scope: !40, file: !1, line: 3)
!42 = !DILocation(line: 3, scope: !40)
