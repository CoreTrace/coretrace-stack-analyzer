; ModuleID = 'test/test_ptrtoint_pattern.c'
source_filename = "test/test_ptrtoint_pattern.c"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

%struct.Data = type { i32, i32, i32 }

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(write, inaccessiblemem: none) uwtable
define dso_local void @test_ptrtoint_pattern() local_unnamed_addr #0 !dbg !21 {
  %1 = alloca %struct.Data, align 4, !DIAssignID !30
  call void @llvm.dbg.assign(metadata i1 undef, metadata !25, metadata !DIExpression(), metadata !30, metadata ptr %1, metadata !DIExpression()), !dbg !31
  call void @llvm.lifetime.start.p0(i64 12, ptr nonnull %1) #4, !dbg !32
  %2 = getelementptr inbounds %struct.Data, ptr %1, i64 0, i32 1, !dbg !33
  tail call void @llvm.dbg.value(metadata ptr %2, metadata !26, metadata !DIExpression()), !dbg !31
  %3 = ptrtoint ptr %2 to i64, !dbg !34
  tail call void @llvm.dbg.value(metadata i64 %3, metadata !28, metadata !DIExpression()), !dbg !31
  %4 = add i64 %3, -8, !dbg !35
  tail call void @llvm.dbg.value(metadata i64 %4, metadata !28, metadata !DIExpression()), !dbg !31
  %5 = inttoptr i64 %4 to ptr, !dbg !36
  tail call void @llvm.dbg.value(metadata ptr %5, metadata !29, metadata !DIExpression()), !dbg !31
  store i32 100, ptr %5, align 4, !dbg !37, !tbaa !38
  call void @llvm.lifetime.end.p0(i64 12, ptr nonnull %1) #4, !dbg !43
  ret void, !dbg !43
}

; Function Attrs: mustprogress nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.start.p0(i64 immarg, ptr nocapture) #1

; Function Attrs: mustprogress nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.end.p0(i64 immarg, ptr nocapture) #1

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(write, inaccessiblemem: none) uwtable
define dso_local noundef i32 @main() local_unnamed_addr #0 !dbg !44 {
  %1 = alloca %struct.Data, align 4, !DIAssignID !47
  call void @llvm.dbg.assign(metadata i1 undef, metadata !25, metadata !DIExpression(), metadata !47, metadata ptr %1, metadata !DIExpression()), !dbg !48
  call void @llvm.lifetime.start.p0(i64 12, ptr nonnull %1) #4, !dbg !50
  %2 = getelementptr inbounds %struct.Data, ptr %1, i64 0, i32 1, !dbg !51
  call void @llvm.dbg.value(metadata ptr %2, metadata !26, metadata !DIExpression()), !dbg !48
  %3 = ptrtoint ptr %2 to i64, !dbg !52
  call void @llvm.dbg.value(metadata i64 %3, metadata !28, metadata !DIExpression()), !dbg !48
  %4 = add i64 %3, -8, !dbg !53
  call void @llvm.dbg.value(metadata i64 %4, metadata !28, metadata !DIExpression()), !dbg !48
  %5 = inttoptr i64 %4 to ptr, !dbg !54
  call void @llvm.dbg.value(metadata ptr %5, metadata !29, metadata !DIExpression()), !dbg !48
  store i32 100, ptr %5, align 4, !dbg !55, !tbaa !38
  call void @llvm.lifetime.end.p0(i64 12, ptr nonnull %1) #4, !dbg !56
  ret i32 0, !dbg !57
}

; Function Attrs: mustprogress nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare void @llvm.dbg.assign(metadata, metadata, metadata, metadata, metadata, metadata) #2

; Function Attrs: nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare void @llvm.dbg.value(metadata, metadata, metadata) #3

attributes #0 = { mustprogress nofree norecurse nosync nounwind willreturn memory(write, inaccessiblemem: none) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #1 = { mustprogress nocallback nofree nosync nounwind willreturn memory(argmem: readwrite) }
attributes #2 = { mustprogress nocallback nofree nosync nounwind speculatable willreturn memory(none) }
attributes #3 = { nocallback nofree nosync nounwind speculatable willreturn memory(none) }
attributes #4 = { nounwind }

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!13, !14, !15, !16, !17, !18, !19}
!llvm.ident = !{!20}

!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, producer: "Ubuntu clang version 18.1.3 (1ubuntu1)", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug, retainedTypes: !2, splitDebugInlining: false, nameTableKind: None)
!1 = !DIFile(filename: "test/test_ptrtoint_pattern.c", directory: "/home/ok/coretrace-stack-analyzer", checksumkind: CSK_MD5, checksum: "b9f87d6e91f8c5aad4fef56a051a06fb")
!2 = !{!3, !6}
!3 = !DIDerivedType(tag: DW_TAG_typedef, name: "uintptr_t", file: !4, line: 79, baseType: !5)
!4 = !DIFile(filename: "/usr/include/stdint.h", directory: "", checksumkind: CSK_MD5, checksum: "bfb03fa9c46a839e35c32b929fbdbb8e")
!5 = !DIBasicType(name: "unsigned long", size: 64, encoding: DW_ATE_unsigned)
!6 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !7, size: 64)
!7 = distinct !DICompositeType(tag: DW_TAG_structure_type, name: "Data", file: !1, line: 4, size: 96, elements: !8)
!8 = !{!9, !11, !12}
!9 = !DIDerivedType(tag: DW_TAG_member, name: "x", scope: !7, file: !1, line: 5, baseType: !10, size: 32)
!10 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!11 = !DIDerivedType(tag: DW_TAG_member, name: "y", scope: !7, file: !1, line: 6, baseType: !10, size: 32, offset: 32)
!12 = !DIDerivedType(tag: DW_TAG_member, name: "z", scope: !7, file: !1, line: 7, baseType: !10, size: 32, offset: 64)
!13 = !{i32 7, !"Dwarf Version", i32 5}
!14 = !{i32 2, !"Debug Info Version", i32 3}
!15 = !{i32 1, !"wchar_size", i32 4}
!16 = !{i32 8, !"PIC Level", i32 2}
!17 = !{i32 7, !"PIE Level", i32 2}
!18 = !{i32 7, !"uwtable", i32 2}
!19 = !{i32 7, !"debug-info-assignment-tracking", i1 true}
!20 = !{!"Ubuntu clang version 18.1.3 (1ubuntu1)"}
!21 = distinct !DISubprogram(name: "test_ptrtoint_pattern", scope: !1, file: !1, line: 10, type: !22, scopeLine: 11, flags: DIFlagPrototyped | DIFlagAllCallsDescribed, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0, retainedNodes: !24)
!22 = !DISubroutineType(types: !23)
!23 = !{null}
!24 = !{!25, !26, !28, !29}
!25 = !DILocalVariable(name: "obj", scope: !21, file: !1, line: 12, type: !7)
!26 = !DILocalVariable(name: "ptr_y", scope: !21, file: !1, line: 13, type: !27)
!27 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !10, size: 64)
!28 = !DILocalVariable(name: "addr", scope: !21, file: !1, line: 17, type: !3)
!29 = !DILocalVariable(name: "wrong_base", scope: !21, file: !1, line: 19, type: !6)
!30 = distinct !DIAssignID()
!31 = !DILocation(line: 0, scope: !21)
!32 = !DILocation(line: 12, column: 5, scope: !21)
!33 = !DILocation(line: 13, column: 23, scope: !21)
!34 = !DILocation(line: 17, column: 22, scope: !21)
!35 = !DILocation(line: 18, column: 10, scope: !21)
!36 = !DILocation(line: 19, column: 31, scope: !21)
!37 = !DILocation(line: 21, column: 19, scope: !21)
!38 = !{!39, !40, i64 0}
!39 = !{!"Data", !40, i64 0, !40, i64 4, !40, i64 8}
!40 = !{!"int", !41, i64 0}
!41 = !{!"omnipotent char", !42, i64 0}
!42 = !{!"Simple C/C++ TBAA"}
!43 = !DILocation(line: 22, column: 1, scope: !21)
!44 = distinct !DISubprogram(name: "main", scope: !1, file: !1, line: 24, type: !45, scopeLine: 24, flags: DIFlagPrototyped | DIFlagAllCallsDescribed, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0)
!45 = !DISubroutineType(types: !46)
!46 = !{!10}
!47 = distinct !DIAssignID()
!48 = !DILocation(line: 0, scope: !21, inlinedAt: !49)
!49 = distinct !DILocation(line: 25, column: 5, scope: !44)
!50 = !DILocation(line: 12, column: 5, scope: !21, inlinedAt: !49)
!51 = !DILocation(line: 13, column: 23, scope: !21, inlinedAt: !49)
!52 = !DILocation(line: 17, column: 22, scope: !21, inlinedAt: !49)
!53 = !DILocation(line: 18, column: 10, scope: !21, inlinedAt: !49)
!54 = !DILocation(line: 19, column: 31, scope: !21, inlinedAt: !49)
!55 = !DILocation(line: 21, column: 19, scope: !21, inlinedAt: !49)
!56 = !DILocation(line: 22, column: 1, scope: !21, inlinedAt: !49)
!57 = !DILocation(line: 26, column: 5, scope: !44)
