; ModuleID = 'test/test_container_of.c'
source_filename = "test/test_container_of.c"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

%struct.MyStruct = type { i32, i32, i32 }

; Function Attrs: noinline nounwind optnone uwtable
define dso_local void @test_invalid_container_of() #0 !dbg !24 {
  %1 = alloca %struct.MyStruct, align 4
  %2 = alloca ptr, align 8
  %3 = alloca ptr, align 8
  call void @llvm.dbg.declare(metadata ptr %1, metadata !28, metadata !DIExpression()), !dbg !29
  call void @llvm.dbg.declare(metadata ptr %2, metadata !30, metadata !DIExpression()), !dbg !32
  %4 = getelementptr inbounds %struct.MyStruct, ptr %1, i32 0, i32 1, !dbg !33
  store ptr %4, ptr %2, align 8, !dbg !32
  call void @llvm.dbg.declare(metadata ptr %3, metadata !34, metadata !DIExpression()), !dbg !35
  %5 = load ptr, ptr %2, align 8, !dbg !36
  %6 = getelementptr inbounds i8, ptr %5, i64 -8, !dbg !37
  store ptr %6, ptr %3, align 8, !dbg !35
  %7 = load ptr, ptr %3, align 8, !dbg !38
  %8 = getelementptr inbounds %struct.MyStruct, ptr %7, i32 0, i32 0, !dbg !39
  store i32 42, ptr %8, align 4, !dbg !40
  ret void, !dbg !41
}

; Function Attrs: nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare void @llvm.dbg.declare(metadata, metadata, metadata) #1

; Function Attrs: noinline nounwind optnone uwtable
define dso_local void @test_valid_container_of() #0 !dbg !42 {
  %1 = alloca %struct.MyStruct, align 4
  %2 = alloca ptr, align 8
  %3 = alloca ptr, align 8
  call void @llvm.dbg.declare(metadata ptr %1, metadata !43, metadata !DIExpression()), !dbg !44
  call void @llvm.dbg.declare(metadata ptr %2, metadata !45, metadata !DIExpression()), !dbg !46
  %4 = getelementptr inbounds %struct.MyStruct, ptr %1, i32 0, i32 1, !dbg !47
  store ptr %4, ptr %2, align 8, !dbg !46
  call void @llvm.dbg.declare(metadata ptr %3, metadata !48, metadata !DIExpression()), !dbg !49
  %5 = load ptr, ptr %2, align 8, !dbg !50
  %6 = getelementptr inbounds i8, ptr %5, i64 -4, !dbg !51
  store ptr %6, ptr %3, align 8, !dbg !49
  %7 = load ptr, ptr %3, align 8, !dbg !52
  %8 = getelementptr inbounds %struct.MyStruct, ptr %7, i32 0, i32 0, !dbg !53
  store i32 42, ptr %8, align 4, !dbg !54
  ret void, !dbg !55
}

; Function Attrs: noinline nounwind optnone uwtable
define dso_local i32 @main() #0 !dbg !56 {
  %1 = alloca i32, align 4
  store i32 0, ptr %1, align 4
  call void @test_invalid_container_of(), !dbg !59
  call void @test_valid_container_of(), !dbg !60
  ret i32 0, !dbg !61
}

attributes #0 = { noinline nounwind optnone uwtable "frame-pointer"="all" "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #1 = { nocallback nofree nosync nounwind speculatable willreturn memory(none) }

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!16, !17, !18, !19, !20, !21, !22}
!llvm.ident = !{!23}

!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, producer: "Ubuntu clang version 18.1.3 (1ubuntu1)", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, retainedTypes: !2, splitDebugInlining: false, nameTableKind: None)
!1 = !DIFile(filename: "test/test_container_of.c", directory: "/home/ok/coretrace-stack-analyzer", checksumkind: CSK_MD5, checksum: "c2c88f002a0cd4be74b04ff6f967db9a")
!2 = !{!3, !14}
!3 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !4, size: 64)
!4 = distinct !DICompositeType(tag: DW_TAG_structure_type, name: "MyStruct", file: !1, line: 4, size: 96, elements: !5)
!5 = !{!6, !12, !13}
!6 = !DIDerivedType(tag: DW_TAG_member, name: "field_a", scope: !4, file: !1, line: 5, baseType: !7, size: 32)
!7 = !DIDerivedType(tag: DW_TAG_typedef, name: "int32_t", file: !8, line: 26, baseType: !9)
!8 = !DIFile(filename: "/usr/include/x86_64-linux-gnu/bits/stdint-intn.h", directory: "", checksumkind: CSK_MD5, checksum: "649b383a60bfa3eb90e85840b2b0be20")
!9 = !DIDerivedType(tag: DW_TAG_typedef, name: "__int32_t", file: !10, line: 41, baseType: !11)
!10 = !DIFile(filename: "/usr/include/x86_64-linux-gnu/bits/types.h", directory: "", checksumkind: CSK_MD5, checksum: "e1865d9fe29fe1b5ced550b7ba458f9e")
!11 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!12 = !DIDerivedType(tag: DW_TAG_member, name: "field_b", scope: !4, file: !1, line: 6, baseType: !7, size: 32, offset: 32)
!13 = !DIDerivedType(tag: DW_TAG_member, name: "field_c", scope: !4, file: !1, line: 7, baseType: !7, size: 32, offset: 64)
!14 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !15, size: 64)
!15 = !DIBasicType(name: "char", size: 8, encoding: DW_ATE_signed_char)
!16 = !{i32 7, !"Dwarf Version", i32 5}
!17 = !{i32 2, !"Debug Info Version", i32 3}
!18 = !{i32 1, !"wchar_size", i32 4}
!19 = !{i32 8, !"PIC Level", i32 2}
!20 = !{i32 7, !"PIE Level", i32 2}
!21 = !{i32 7, !"uwtable", i32 2}
!22 = !{i32 7, !"frame-pointer", i32 2}
!23 = !{!"Ubuntu clang version 18.1.3 (1ubuntu1)"}
!24 = distinct !DISubprogram(name: "test_invalid_container_of", scope: !1, file: !1, line: 10, type: !25, scopeLine: 11, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !27)
!25 = !DISubroutineType(types: !26)
!26 = !{null}
!27 = !{}
!28 = !DILocalVariable(name: "obj", scope: !24, file: !1, line: 12, type: !4)
!29 = !DILocation(line: 12, column: 21, scope: !24)
!30 = !DILocalVariable(name: "ptr_b", scope: !24, file: !1, line: 13, type: !31)
!31 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !7, size: 64)
!32 = !DILocation(line: 13, column: 14, scope: !24)
!33 = !DILocation(line: 13, column: 27, scope: !24)
!34 = !DILocalVariable(name: "wrong_base", scope: !24, file: !1, line: 16, type: !3)
!35 = !DILocation(line: 16, column: 22, scope: !24)
!36 = !DILocation(line: 16, column: 63, scope: !24)
!37 = !DILocation(line: 16, column: 69, scope: !24)
!38 = !DILocation(line: 18, column: 5, scope: !24)
!39 = !DILocation(line: 18, column: 17, scope: !24)
!40 = !DILocation(line: 18, column: 25, scope: !24)
!41 = !DILocation(line: 19, column: 1, scope: !24)
!42 = distinct !DISubprogram(name: "test_valid_container_of", scope: !1, file: !1, line: 21, type: !25, scopeLine: 22, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !27)
!43 = !DILocalVariable(name: "obj", scope: !42, file: !1, line: 23, type: !4)
!44 = !DILocation(line: 23, column: 21, scope: !42)
!45 = !DILocalVariable(name: "ptr_b", scope: !42, file: !1, line: 24, type: !31)
!46 = !DILocation(line: 24, column: 14, scope: !42)
!47 = !DILocation(line: 24, column: 27, scope: !42)
!48 = !DILocalVariable(name: "correct_base", scope: !42, file: !1, line: 27, type: !3)
!49 = !DILocation(line: 27, column: 22, scope: !42)
!50 = !DILocation(line: 27, column: 65, scope: !42)
!51 = !DILocation(line: 27, column: 71, scope: !42)
!52 = !DILocation(line: 29, column: 5, scope: !42)
!53 = !DILocation(line: 29, column: 19, scope: !42)
!54 = !DILocation(line: 29, column: 27, scope: !42)
!55 = !DILocation(line: 30, column: 1, scope: !42)
!56 = distinct !DISubprogram(name: "main", scope: !1, file: !1, line: 32, type: !57, scopeLine: 33, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !0)
!57 = !DISubroutineType(types: !58)
!58 = !{!11}
!59 = !DILocation(line: 34, column: 5, scope: !56)
!60 = !DILocation(line: 35, column: 5, scope: !56)
!61 = !DILocation(line: 36, column: 5, scope: !56)
