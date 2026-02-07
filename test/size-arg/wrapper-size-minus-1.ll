; ModuleID = '/tmp/coretrace-stack-analyzer/test/size-arg/wrapper-size-minus-1.c'
source_filename = "/tmp/coretrace-stack-analyzer/test/size-arg/wrapper-size-minus-1.c"
target datalayout = "e-m:o-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx15.0.0"

; Function Attrs: noinline nounwind optnone ssp uwtable(sync)
define void @test2(ptr noundef %dst, ptr noundef %src, i64 noundef %n) #0 !dbg !10 {
entry:
  %dst.addr = alloca ptr, align 8
  %src.addr = alloca ptr, align 8
  %n.addr = alloca i64, align 8
  store ptr %dst, ptr %dst.addr, align 8
    #dbg_declare(ptr %dst.addr, !24, !DIExpression(), !25)
  store ptr %src, ptr %src.addr, align 8
    #dbg_declare(ptr %src.addr, !26, !DIExpression(), !27)
  store i64 %n, ptr %n.addr, align 8
    #dbg_declare(ptr %n.addr, !28, !DIExpression(), !29)
  %0 = load ptr, ptr %dst.addr, align 8, !dbg !30
  %1 = load ptr, ptr %src.addr, align 8, !dbg !30
  %2 = load i64, ptr %n.addr, align 8, !dbg !30
  %3 = load ptr, ptr %dst.addr, align 8, !dbg !30
  %4 = call i64 @llvm.objectsize.i64.p0(ptr %3, i1 false, i1 true, i1 false), !dbg !30
  %call = call ptr @__strncpy_chk(ptr noundef %0, ptr noundef %1, i64 noundef %2, i64 noundef %4) #3, !dbg !30
  ret void, !dbg !31
}

; Function Attrs: nounwind
declare ptr @__strncpy_chk(ptr noundef, ptr noundef, i64 noundef, i64 noundef) #1

; Function Attrs: nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare i64 @llvm.objectsize.i64.p0(ptr, i1 immarg, i1 immarg, i1 immarg) #2

; Function Attrs: noinline nounwind optnone ssp uwtable(sync)
define void @test(ptr noundef %dst, ptr noundef %src, i64 noundef %n) #0 !dbg !32 {
entry:
  %dst.addr = alloca ptr, align 8
  %src.addr = alloca ptr, align 8
  %n.addr = alloca i64, align 8
  store ptr %dst, ptr %dst.addr, align 8
    #dbg_declare(ptr %dst.addr, !33, !DIExpression(), !34)
  store ptr %src, ptr %src.addr, align 8
    #dbg_declare(ptr %src.addr, !35, !DIExpression(), !36)
  store i64 %n, ptr %n.addr, align 8
    #dbg_declare(ptr %n.addr, !37, !DIExpression(), !38)
  %0 = load ptr, ptr %dst.addr, align 8, !dbg !39
  %1 = load ptr, ptr %src.addr, align 8, !dbg !40
  %2 = load i64, ptr %n.addr, align 8, !dbg !41
  call void @test2(ptr noundef %0, ptr noundef %1, i64 noundef %2), !dbg !42
  ret void, !dbg !43
}

; Function Attrs: noinline nounwind optnone ssp uwtable(sync)
define void @caller(ptr noundef %dst, ptr noundef %src, i64 noundef %n) #0 !dbg !44 {
entry:
  %dst.addr = alloca ptr, align 8
  %src.addr = alloca ptr, align 8
  %n.addr = alloca i64, align 8
  store ptr %dst, ptr %dst.addr, align 8
    #dbg_declare(ptr %dst.addr, !45, !DIExpression(), !46)
  store ptr %src, ptr %src.addr, align 8
    #dbg_declare(ptr %src.addr, !47, !DIExpression(), !48)
  store i64 %n, ptr %n.addr, align 8
    #dbg_declare(ptr %n.addr, !49, !DIExpression(), !50)
  %0 = load ptr, ptr %dst.addr, align 8, !dbg !51
  %1 = load ptr, ptr %src.addr, align 8, !dbg !52
  %2 = load i64, ptr %n.addr, align 8, !dbg !53
  %sub = sub i64 %2, 1, !dbg !54
  call void @test(ptr noundef %0, ptr noundef %1, i64 noundef %sub), !dbg !55
  ret void, !dbg !56
}

attributes #0 = { noinline nounwind optnone ssp uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "probe-stack"="__chkstk_darwin" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+bti,+ccdp,+ccidx,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8.5a,+v8a,+zcm,+zcz" }
attributes #1 = { nounwind "frame-pointer"="non-leaf" "no-trapping-math"="true" "probe-stack"="__chkstk_darwin" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+bti,+ccdp,+ccidx,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8.5a,+v8a,+zcm,+zcz" }
attributes #2 = { nocallback nofree nosync nounwind speculatable willreturn memory(none) }
attributes #3 = { nounwind }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6}
!llvm.dbg.cu = !{!7}
!llvm.ident = !{!9}

!0 = !{i32 2, !"SDK Version", [2 x i32] [i32 26, i32 2]}
!1 = !{i32 7, !"Dwarf Version", i32 5}
!2 = !{i32 2, !"Debug Info Version", i32 3}
!3 = !{i32 1, !"wchar_size", i32 4}
!4 = !{i32 8, !"PIC Level", i32 2}
!5 = !{i32 7, !"uwtable", i32 1}
!6 = !{i32 7, !"frame-pointer", i32 1}
!7 = distinct !DICompileUnit(language: DW_LANG_C11, file: !8, producer: "Apple clang version 17.0.0 (clang-1700.6.3.2)", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, splitDebugInlining: false, nameTableKind: Apple, sysroot: "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk", sdk: "MacOSX.sdk")
!8 = !DIFile(filename: "/tmp/coretrace-stack-analyzer/test/size-arg/wrapper-size-minus-1.c", directory: "/private/tmp", checksumkind: CSK_MD5, checksum: "1a0b03385c639f79a4276f293c4423f0")
!9 = !{!"Apple clang version 17.0.0 (clang-1700.6.3.2)"}
!10 = distinct !DISubprogram(name: "test2", scope: !11, file: !11, line: 4, type: !12, scopeLine: 5, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !7, retainedNodes: !23)
!11 = !DIFile(filename: "/tmp/coretrace-stack-analyzer/test/size-arg/wrapper-size-minus-1.c", directory: "", checksumkind: CSK_MD5, checksum: "1a0b03385c639f79a4276f293c4423f0")
!12 = !DISubroutineType(types: !13)
!13 = !{null, !14, !16, !18}
!14 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !15, size: 64)
!15 = !DIBasicType(name: "char", size: 8, encoding: DW_ATE_signed_char)
!16 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !17, size: 64)
!17 = !DIDerivedType(tag: DW_TAG_const_type, baseType: !15)
!18 = !DIDerivedType(tag: DW_TAG_typedef, name: "size_t", file: !19, line: 50, baseType: !20)
!19 = !DIFile(filename: "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include/sys/_types/_size_t.h", directory: "", checksumkind: CSK_MD5, checksum: "f7981334d28e0c246f35cd24042aa2a4")
!20 = !DIDerivedType(tag: DW_TAG_typedef, name: "__darwin_size_t", file: !21, line: 87, baseType: !22)
!21 = !DIFile(filename: "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include/arm/_types.h", directory: "", checksumkind: CSK_MD5, checksum: "b270144f57ae258d0ce80b8f87be068c")
!22 = !DIBasicType(name: "unsigned long", size: 64, encoding: DW_ATE_unsigned)
!23 = !{}
!24 = !DILocalVariable(name: "dst", arg: 1, scope: !10, file: !11, line: 4, type: !14)
!25 = !DILocation(line: 4, column: 18, scope: !10)
!26 = !DILocalVariable(name: "src", arg: 2, scope: !10, file: !11, line: 4, type: !16)
!27 = !DILocation(line: 4, column: 35, scope: !10)
!28 = !DILocalVariable(name: "n", arg: 3, scope: !10, file: !11, line: 4, type: !18)
!29 = !DILocation(line: 4, column: 47, scope: !10)
!30 = !DILocation(line: 6, column: 5, scope: !10)
!31 = !DILocation(line: 7, column: 1, scope: !10)
!32 = distinct !DISubprogram(name: "test", scope: !11, file: !11, line: 9, type: !12, scopeLine: 10, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !7, retainedNodes: !23)
!33 = !DILocalVariable(name: "dst", arg: 1, scope: !32, file: !11, line: 9, type: !14)
!34 = !DILocation(line: 9, column: 17, scope: !32)
!35 = !DILocalVariable(name: "src", arg: 2, scope: !32, file: !11, line: 9, type: !16)
!36 = !DILocation(line: 9, column: 34, scope: !32)
!37 = !DILocalVariable(name: "n", arg: 3, scope: !32, file: !11, line: 9, type: !18)
!38 = !DILocation(line: 9, column: 46, scope: !32)
!39 = !DILocation(line: 11, column: 11, scope: !32)
!40 = !DILocation(line: 11, column: 16, scope: !32)
!41 = !DILocation(line: 11, column: 21, scope: !32)
!42 = !DILocation(line: 11, column: 5, scope: !32)
!43 = !DILocation(line: 12, column: 1, scope: !32)
!44 = distinct !DISubprogram(name: "caller", scope: !11, file: !11, line: 14, type: !12, scopeLine: 15, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !7, retainedNodes: !23)
!45 = !DILocalVariable(name: "dst", arg: 1, scope: !44, file: !11, line: 14, type: !14)
!46 = !DILocation(line: 14, column: 19, scope: !44)
!47 = !DILocalVariable(name: "src", arg: 2, scope: !44, file: !11, line: 14, type: !16)
!48 = !DILocation(line: 14, column: 36, scope: !44)
!49 = !DILocalVariable(name: "n", arg: 3, scope: !44, file: !11, line: 14, type: !18)
!50 = !DILocation(line: 14, column: 48, scope: !44)
!51 = !DILocation(line: 20, column: 10, scope: !44)
!52 = !DILocation(line: 20, column: 15, scope: !44)
!53 = !DILocation(line: 20, column: 20, scope: !44)
!54 = !DILocation(line: 20, column: 22, scope: !44)
!55 = !DILocation(line: 20, column: 5, scope: !44)
!56 = !DILocation(line: 21, column: 1, scope: !44)
