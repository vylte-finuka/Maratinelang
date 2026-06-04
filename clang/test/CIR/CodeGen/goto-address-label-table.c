// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-cir %s -o %t.cir
// RUN: FileCheck --input-file=%t.cir %s --check-prefix=CIR
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm %s -o %t-cir.ll
// RUN: FileCheck --input-file=%t-cir.ll %s --check-prefix=LLVM
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o %t.ll
// RUN: FileCheck --input-file=%t.ll %s --check-prefix=OGCG

int f(int x) {
  static const void *tbl[] = {&&L1, &&L2};
  goto *tbl[x];
L1:
  return 1;
L2:
  return 2;
}

// A appears twice in g's table; both occurrences are kept as indirect-branch
// successors, matching classic codegen.
int g(int x) {
  static const void *tbl[] = {&&A, &&A, &&B};
  goto *tbl[x];
A:
  return 1;
B:
  return 2;
}

// CIR-DAG: cir.global "private" internal dso_local @f.tbl = #cir.const_array<[#cir.block_address<@f, "L1"> : !cir.ptr<!void>, #cir.block_address<@f, "L2"> : !cir.ptr<!void>]> : !cir.array<!cir.ptr<!void> x 2>
// CIR-DAG: cir.global "private" internal dso_local @g.tbl = #cir.const_array<[#cir.block_address<@g, "A"> : !cir.ptr<!void>, #cir.block_address<@g, "A"> : !cir.ptr<!void>, #cir.block_address<@g, "B"> : !cir.ptr<!void>]> : !cir.array<!cir.ptr<!void> x 3>

// CIR: cir.func {{.*}} @f
// CIR:   %[[TBL:.*]] = cir.get_global @f.tbl
// CIR:   %[[ELT:.*]] = cir.get_element %[[TBL]]
// CIR:   %[[ADDR:.*]] = cir.load align(8) %[[ELT]] : !cir.ptr<!cir.ptr<!void>>, !cir.ptr<!void>
// CIR:   cir.br ^[[INDIRECT:.*]](%[[ADDR]] : !cir.ptr<!void>)
// CIR: ^[[INDIRECT]](%[[PHI:.*]]: !cir.ptr<!void> {{.*}}):
// CIR:   cir.indirect_br %[[PHI]] : !cir.ptr<!void>, [
// CIR-NEXT: ^[[L1BB:.*]],
// CIR-NEXT: ^[[L2BB:.*]]
// CIR:   ]
// CIR: ^[[L1BB]]:
// CIR:   cir.label "L1"
// CIR: ^[[L2BB]]:
// CIR:   cir.label "L2"

// CIR: cir.func {{.*}} @g
// CIR:   cir.indirect_br %{{.*}} : !cir.ptr<!void>, [
// CIR-NEXT: ^[[ABB:.*]],
// CIR-NEXT: ^[[ABB]],
// CIR-NEXT: ^[[BBB:.*]]
// CIR:   ]
// CIR: ^[[ABB]]:
// CIR:   cir.label "A"
// CIR: ^[[BBB]]:
// CIR:   cir.label "B"

// LLVM-DAG: @f.tbl = internal global [2 x ptr] [ptr blockaddress(@f, %[[L1:[0-9]+]]), ptr blockaddress(@f, %[[L2:[0-9]+]])], align 16
// LLVM-DAG: @g.tbl = internal global [3 x ptr] [ptr blockaddress(@g, %[[GA:[0-9]+]]), ptr blockaddress(@g, %[[GA]]), ptr blockaddress(@g, %[[GB:[0-9]+]])], align 16

// LLVM: define dso_local i32 @f(i32 noundef %{{.*}})
// LLVM:   %[[IDX:.*]] = getelementptr [2 x ptr], ptr @f.tbl, i32 0, i64 %{{.*}}
// LLVM:   %[[ADDR:.*]] = load ptr, ptr %[[IDX]], align 8
// LLVM:   br label %[[IGOTO:[0-9]+]]
// LLVM: [[IGOTO]]:
// LLVM:   %[[PHI:.*]] = phi ptr [ %[[ADDR]], %{{.*}} ]
// LLVM:   indirectbr ptr %[[PHI]], [label %[[L1]], label %[[L2]]]
// LLVM: [[L1]]:
// LLVM:   store i32 1, ptr %{{.*}}, align 4
// LLVM: [[L2]]:
// LLVM:   store i32 2, ptr %{{.*}}, align 4

// LLVM: define dso_local i32 @g(i32 noundef %{{.*}})
// LLVM:   indirectbr ptr %{{.*}}, [label %[[GA]], label %[[GA]], label %[[GB]]]

// OGCG-DAG: @f.tbl = internal global [2 x ptr] [ptr blockaddress(@f, %L1), ptr blockaddress(@f, %L2)], align 16
// OGCG-DAG: @g.tbl = internal global [3 x ptr] [ptr blockaddress(@g, %A), ptr blockaddress(@g, %A), ptr blockaddress(@g, %B)], align 16

// OGCG: define dso_local i32 @f(i32 noundef %{{.*}})
// OGCG:   %[[ARRAYIDX:.*]] = getelementptr inbounds [2 x ptr], ptr @f.tbl, i64 0, i64 %{{.*}}
// OGCG:   %[[ADDR:.*]] = load ptr, ptr %[[ARRAYIDX]], align 8
// OGCG:   br label %indirectgoto
// OGCG: L1:
// OGCG:   store i32 1, ptr %{{.*}}, align 4
// OGCG: L2:
// OGCG:   store i32 2, ptr %{{.*}}, align 4
// OGCG: indirectgoto:
// OGCG:   %indirect.goto.dest = phi ptr [ %{{.*}}, %entry ]
// OGCG:   indirectbr ptr %indirect.goto.dest, [label %L1, label %L2]

// OGCG: define dso_local i32 @g(i32 noundef %{{.*}})
// OGCG:   indirectbr ptr %indirect.goto.dest, [label %A, label %A, label %B]
