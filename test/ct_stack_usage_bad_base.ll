; ModuleID = 'test/ct_stack_usage_bad_base.c'
source_filename = "test/ct_stack_usage_bad_base.c"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

%struct.A = type { i32, i32, i32, i32 }

@__const.main.obj = private unnamed_addr constant %struct.A { i32 11, i32 22, i32 33, i32 44 }, align 4
@.str = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1, !dbg !0

; Function Attrs: noinline nounwind optnone uwtable
define dso_local i32 @main() #0 !dbg !31 {
  %1 = alloca i32, align 4
  %2 = alloca %struct.A, align 4
  %3 = alloca ptr, align 8
  %4 = alloca ptr, align 8
  store i32 0, ptr %1, align 4
  call void @llvm.dbg.declare(metadata ptr %2, metadata !35, metadata !DIExpression()), !dbg !36
  call void @llvm.memcpy.p0.p0.i64(ptr align 4 %2, ptr align 4 @__const.main.obj, i64 16, i1 false), !dbg !36
  call void @llvm.dbg.declare(metadata ptr %3, metadata !37, metadata !DIExpression()), !dbg !39
  %5 = getelementptr inbounds %struct.A, ptr %2, i32 0, i32 1, !dbg !40
  store ptr %5, ptr %3, align 8, !dbg !39
  call void @llvm.dbg.declare(metadata ptr %4, metadata !41, metadata !DIExpression()), !dbg !42
  %6 = load ptr, ptr %3, align 8, !dbg !43
  %7 = getelementptr inbounds i8, ptr %6, i64 -12, !dbg !44
  store ptr %7, ptr %4, align 8, !dbg !42
  %8 = load ptr, ptr %4, align 8, !dbg !45
  %9 = getelementptr inbounds %struct.A, ptr %8, i32 0, i32 0, !dbg !46
  %10 = load i32, ptr %9, align 4, !dbg !46
  %11 = call i32 (ptr, ...) @printf(ptr noundef @.str, i32 noundef %10), !dbg !47
  %12 = load ptr, ptr %4, align 8, !dbg !48
  %13 = getelementptr inbounds %struct.A, ptr %12, i32 0, i32 3, !dbg !49
  %14 = load i32, ptr %13, align 4, !dbg !49
  %15 = call i32 (ptr, ...) @printf(ptr noundef @.str, i32 noundef %14), !dbg !50
  ret i32 0, !dbg !51
}

; Function Attrs: nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare void @llvm.dbg.declare(metadata, metadata, metadata) #1

; Function Attrs: nocallback nofree nounwind willreturn memory(argmem: readwrite)
declare void @llvm.memcpy.p0.p0.i64(ptr noalias nocapture writeonly, ptr noalias nocapture readonly, i64, i1 immarg) #2

declare i32 @printf(ptr noundef, ...) #3

attributes #0 = { noinline nounwind optnone uwtable "frame-pointer"="all" "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #1 = { nocallback nofree nosync nounwind speculatable willreturn memory(none) }
attributes #2 = { nocallback nofree nounwind willreturn memory(argmem: readwrite) }
attributes #3 = { "frame-pointer"="all" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }

!llvm.dbg.cu = !{!7}
!llvm.module.flags = !{!23, !24, !25, !26, !27, !28, !29}
!llvm.ident = !{!30}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(scope: null, file: !2, line: 22, type: !3, isLocal: true, isDefinition: true)
!2 = !DIFile(filename: "test/ct_stack_usage_bad_base.c", directory: "/home/ok/coretrace-stack-analyzer", checksumkind: CSK_MD5, checksum: "070a7509005af0e6afc160d6f50027c4")
!3 = !DICompositeType(tag: DW_TAG_array_type, baseType: !4, size: 32, elements: !5)
!4 = !DIBasicType(name: "char", size: 8, encoding: DW_ATE_signed_char)
!5 = !{!6}
!6 = !DISubrange(count: 4)
!7 = distinct !DICompileUnit(language: DW_LANG_C11, file: !2, producer: "Ubuntu clang version 18.1.3 (1ubuntu1)", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, retainedTypes: !8, globals: !22, splitDebugInlining: false, nameTableKind: None)
!8 = !{!9, !21}
!9 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !10, size: 64)
!10 = distinct !DICompositeType(tag: DW_TAG_structure_type, name: "A", file: !2, line: 5, size: 128, elements: !11)
!11 = !{!12, !18, !19, !20}
!12 = !DIDerivedType(tag: DW_TAG_member, name: "a", scope: !10, file: !2, line: 6, baseType: !13, size: 32)
!13 = !DIDerivedType(tag: DW_TAG_typedef, name: "int32_t", file: !14, line: 26, baseType: !15)
!14 = !DIFile(filename: "/usr/include/x86_64-linux-gnu/bits/stdint-intn.h", directory: "", checksumkind: CSK_MD5, checksum: "649b383a60bfa3eb90e85840b2b0be20")
!15 = !DIDerivedType(tag: DW_TAG_typedef, name: "__int32_t", file: !16, line: 41, baseType: !17)
!16 = !DIFile(filename: "/usr/include/x86_64-linux-gnu/bits/types.h", directory: "", checksumkind: CSK_MD5, checksum: "e1865d9fe29fe1b5ced550b7ba458f9e")
!17 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!18 = !DIDerivedType(tag: DW_TAG_member, name: "b", scope: !10, file: !2, line: 7, baseType: !13, size: 32, offset: 32)
!19 = !DIDerivedType(tag: DW_TAG_member, name: "c", scope: !10, file: !2, line: 8, baseType: !13, size: 32, offset: 64)
!20 = !DIDerivedType(tag: DW_TAG_member, name: "i", scope: !10, file: !2, line: 9, baseType: !13, size: 32, offset: 96)
!21 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !4, size: 64)
!22 = !{!0}
!23 = !{i32 7, !"Dwarf Version", i32 5}
!24 = !{i32 2, !"Debug Info Version", i32 3}
!25 = !{i32 1, !"wchar_size", i32 4}
!26 = !{i32 8, !"PIC Level", i32 2}
!27 = !{i32 7, !"PIE Level", i32 2}
!28 = !{i32 7, !"uwtable", i32 2}
!29 = !{i32 7, !"frame-pointer", i32 2}
!30 = !{!"Ubuntu clang version 18.1.3 (1ubuntu1)"}
!31 = distinct !DISubprogram(name: "main", scope: !2, file: !2, line: 12, type: !32, scopeLine: 13, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !7, retainedNodes: !34)
!32 = !DISubroutineType(types: !33)
!33 = !{!17}
!34 = !{}
!35 = !DILocalVariable(name: "obj", scope: !31, file: !2, line: 14, type: !10)
!36 = !DILocation(line: 14, column: 14, scope: !31)
!37 = !DILocalVariable(name: "pb", scope: !31, file: !2, line: 16, type: !38)
!38 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !13, size: 64)
!39 = !DILocation(line: 16, column: 14, scope: !31)
!40 = !DILocation(line: 16, column: 24, scope: !31)
!41 = !DILocalVariable(name: "bad_base", scope: !31, file: !2, line: 19, type: !9)
!42 = !DILocation(line: 19, column: 15, scope: !31)
!43 = !DILocation(line: 19, column: 47, scope: !31)
!44 = !DILocation(line: 19, column: 50, scope: !31)
!45 = !DILocation(line: 22, column: 20, scope: !31)
!46 = !DILocation(line: 22, column: 30, scope: !31)
!47 = !DILocation(line: 22, column: 5, scope: !31)
!48 = !DILocation(line: 23, column: 20, scope: !31)
!49 = !DILocation(line: 23, column: 30, scope: !31)
!50 = !DILocation(line: 23, column: 5, scope: !31)
!51 = !DILocation(line: 24, column: 5, scope: !31)
