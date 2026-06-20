// Vyft Ltd — Mara/Maratine Code Generator Implementation — Proprietary — 2026

#include "MaratineCodeGen.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::maratine;

CodeGenerator::CodeGenerator(StringRef ModuleName, StringRef Arch)
    : Ctx(std::make_unique<LLVMContext>()),
      Mod(std::make_unique<llvm::Module>(ModuleName, *Ctx)),
      Builder(std::make_unique<IRBuilder<>>(*Ctx)) {

  // Triple et DataLayout selon l'architecture cible
  if (Arch == "x64") {
    Mod->setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
    Mod->setTargetTriple(llvm::Triple("x86_64-unknown-linux-gnu"));
    Mod->setDataLayout(
        "e-m:w-p270:32:32-p271:32:32-p272:64:64"
        "-i64:64-i128:128-f80:128-n8:16:32:64-S128");
  } else {
    // arm64 — Slura OS / Exynos W1000 (Cortex-A78/A55)
    Mod->setTargetTriple(llvm::Triple("aarch64-unknown-none-elf"));
    Mod->setDataLayout(
        "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128");
  }

  initRuntime();
}

void CodeGenerator::initRuntime() {
  // Declare printf for log: — takes a char* and returns i32
  std::vector<llvm::Type *> Args = { Builder->getPtrTy() };
  auto *FT = llvm::FunctionType::get(Builder->getInt32Ty(), Args, /*isVarArg=*/true);
  LogFunc = llvm::Function::Create(FT, GlobalValue::ExternalLinkage, "printf", Mod.get());
}

llvm::Type *CodeGenerator::maraTypeToLLVM(MaraTypeKind K) {
  switch (K) {
    case MaraTypeKind::I32:      return Builder->getInt32Ty();
    case MaraTypeKind::I64:      return Builder->getInt64Ty();
    case MaraTypeKind::U64:      return Builder->getInt64Ty();
    case MaraTypeKind::Bool:     return Builder->getInt1Ty();
    case MaraTypeKind::Ptr:      return Builder->getPtrTy();
    case MaraTypeKind::String:   return Builder->getPtrTy();
    case MaraTypeKind::Array:    return Builder->getPtrTy();
    case MaraTypeKind::Compound: return Builder->getPtrTy();
    default:                     return Builder->getInt32Ty();
  }
}

GlobalValue::LinkageTypes CodeGenerator::visToLinkage(Visibility V) {
  return (V == Visibility::Public) ? GlobalValue::ExternalLinkage
                                   : GlobalValue::InternalLinkage;
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------
Expected<llvm::Value *> CodeGenerator::codegenExpr(Expr *E) {
  if (auto *SL = dyn_cast<StringLiteral>(E))  return codegenStringLit(SL);
  if (auto *IL = dyn_cast<IntLiteral>(E))     return codegenIntLit(IL);
  if (auto *BL = dyn_cast<BoolLiteral>(E))    return codegenBoolLit(BL);
  if (auto *NL = dyn_cast<NullLiteral>(E))    return codegenNullLit(NL);
  if (auto *VR = dyn_cast<VarRef>(E))         return codegenVarRef(VR);
  if (auto *CE = dyn_cast<CallExpr>(E))       return codegenCallExpr(CE);
  if (auto *FC = dyn_cast<FFICallExpr>(E))    return codegenFFICall(FC);
  if (auto *BE = dyn_cast<BinaryExpr>(E))     return codegenBinaryExpr(BE);
  if (auto *AL = dyn_cast<ArrayLiteral>(E))   return codegenArrayLiteral(AL);
  return createStringError(inconvertibleErrorCode(), "unknown expression type");
}

Expected<llvm::Value *> CodeGenerator::codegenStringLit(StringLiteral *E) {
  return Builder->CreateGlobalString(E->Value);
}

Expected<llvm::Value *> CodeGenerator::codegenIntLit(IntLiteral *E) {
  if (E->IntTyKind == MaraTypeKind::I64 || E->IntTyKind == MaraTypeKind::U64)
    return ConstantInt::get(Builder->getInt64Ty(), (uint64_t)E->Value);
  return ConstantInt::get(Builder->getInt32Ty(), (uint32_t)E->Value);
}

Expected<llvm::Value *> CodeGenerator::codegenBoolLit(BoolLiteral *E) {
  return ConstantInt::get(Builder->getInt1Ty(), E->Value ? 1 : 0);
}

Expected<llvm::Value *> CodeGenerator::codegenNullLit(NullLiteral *) {
  return ConstantPointerNull::get(Builder->getPtrTy());
}

Expected<llvm::Value *> CodeGenerator::codegenVarRef(VarRef *E) {
  auto It = Syms.find(E->Name);
  if (It == Syms.end()) {
    // Try last segment of a path name (e.g. self***varName)
    std::string Short = E->Name;
    auto P = Short.rfind("***");
    if (P != std::string::npos) {
      Short = Short.substr(P + 3);
      It = Syms.find(Short);
    }
  }
  if (It == Syms.end())
    return createStringError(inconvertibleErrorCode(),
                             "undefined variable '" + E->Name + "'");
  llvm::Type *LoadTy = Builder->getInt32Ty();
  if (auto *AI = dyn_cast<AllocaInst>(It->second))
    LoadTy = AI->getAllocatedType();
  else if (auto *GV = dyn_cast<GlobalVariable>(It->second))
    LoadTy = GV->getValueType();
  return Builder->CreateLoad(LoadTy, It->second, E->Name);
}

Expected<llvm::Value *> CodeGenerator::codegenCallExpr(CallExpr *E) {
  // Try exact name first, then last segment (self***method or Module***func)
  std::string CallName = E->FunctionName;
  llvm::Function *F = Mod->getFunction(CallName);
  if (!F) {
    auto P = CallName.rfind("***");
    if (P != std::string::npos) {
      std::string Last = CallName.substr(P + 3);
      F = Mod->getFunction(Last);
    }
  }
  if (!F) {
    // Not defined in this module — declare as external (resolved at link time)
    std::string Mangled = CallName;
    for (size_t I = 0; (I = Mangled.find("***", I)) != std::string::npos; )
      Mangled.replace(I, 3, "___");
    F = Mod->getFunction(Mangled);
    if (!F) {
      std::vector<llvm::Type *> PTs(E->Args.size(), Builder->getPtrTy());
      auto *FT = llvm::FunctionType::get(Builder->getInt32Ty(), PTs, false);
      F = llvm::Function::Create(FT, GlobalValue::ExternalLinkage, Mangled, Mod.get());
    }
  }
  std::vector<llvm::Value *> ArgsV;
  llvm::FunctionType *FT = F->getFunctionType();
  for (size_t I = 0; I < E->Args.size(); ++I) {
    auto V = codegenExpr(E->Args[I].get());
    if (!V) return V.takeError();
    llvm::Value *Val = *V;
    if (I < FT->getNumParams()) {
      llvm::Type *ParamTy = FT->getParamType(I);
      if (Val->getType() != ParamTy) {
        if (ParamTy->isPointerTy() && Val->getType()->isIntegerTy())
          Val = Builder->CreateIntToPtr(Val, ParamTy);
        else if (ParamTy->isIntegerTy() && Val->getType()->isPointerTy())
          Val = Builder->CreatePtrToInt(Val, ParamTy);
        else if (ParamTy->isIntegerTy() && Val->getType()->isIntegerTy())
          Val = Builder->CreateIntCast(Val, ParamTy, /*isSigned=*/true);
        else if (ParamTy->isPointerTy() && Val->getType()->isPointerTy())
          Val = Builder->CreatePointerCast(Val, ParamTy);
      }
    }
    ArgsV.push_back(Val);
  }
  return Builder->CreateCall(F, ArgsV);
}

Expected<llvm::Value *> CodeGenerator::codegenFFICall(FFICallExpr *E) {
  // FFI functions are resolved at link time via DrvAPIInterCon.
  // We declare an external function with the mangled path name.
  std::string MangledName = E->Path;
  // Replace *** with ___ for a valid symbol name
  for (size_t I = 0; (I = MangledName.find("***", I)) != std::string::npos; )
    MangledName.replace(I, 3, "___");

  llvm::Function *F = Mod->getFunction(MangledName);
  if (!F) {
    std::vector<llvm::Type *> ParamTypes(E->Args.size(), Builder->getPtrTy());
    auto *FT = llvm::FunctionType::get(Builder->getInt32Ty(), ParamTypes, false);
    F = llvm::Function::Create(FT, GlobalValue::ExternalLinkage, MangledName, Mod.get());
  }

  std::vector<llvm::Value *> ArgsV;
  llvm::Type *PtrTy = Builder->getPtrTy();
  for (auto &A : E->Args) {
    auto V = codegenExpr(A.get()); if (!V) return V.takeError();
    llvm::Value *Val = *V;
    if (Val->getType()->isIntegerTy())
      Val = Builder->CreateIntToPtr(Val, PtrTy);
    else if (Val->getType()->isPointerTy() && Val->getType() != PtrTy)
      Val = Builder->CreatePointerCast(Val, PtrTy);
    ArgsV.push_back(Val);
  }
  return Builder->CreateCall(F, ArgsV);
}

Expected<llvm::Value *> CodeGenerator::codegenBinaryExpr(BinaryExpr *E) {
  auto L = codegenExpr(E->LHS.get()); if (!L) return L.takeError();
  auto R = codegenExpr(E->RHS.get()); if (!R) return R.takeError();
  llvm::Value *LV = *L;
  llvm::Value *RV = *R;
  StringRef Op = E->Op;

  if (Op == "+") {
    llvm::Type *LTy = LV->getType();
    llvm::Type *RTy = RV->getType();
    // String concatenation: ptr + int → GEP (valid IR, runtime concatenation via log)
    if (LTy->isPointerTy() && RTy->isIntegerTy())
      return Builder->CreateGEP(Builder->getInt8Ty(), LV, RV);
    if (LTy->isIntegerTy() && RTy->isPointerTy())
      return Builder->CreateGEP(Builder->getInt8Ty(), RV, LV);
    if (LTy->isPointerTy() && RTy->isPointerTy())
      return LV;
    // Integer add — widen narrower operand if needed
    if (LTy != RTy && LTy->isIntegerTy() && RTy->isIntegerTy()) {
      if (LTy->getIntegerBitWidth() < RTy->getIntegerBitWidth())
        LV = Builder->CreateSExt(LV, RTy);
      else
        RV = Builder->CreateSExt(RV, LTy);
    }
    return Builder->CreateAdd(LV, RV);
  }

  // Normalize integer operand types for all other operators
  if (LV->getType()->isIntegerTy() && RV->getType()->isIntegerTy() &&
      LV->getType() != RV->getType()) {
    if (LV->getType()->getIntegerBitWidth() < RV->getType()->getIntegerBitWidth())
      LV = Builder->CreateSExt(LV, RV->getType());
    else
      RV = Builder->CreateSExt(RV, LV->getType());
  }
  // Normalize ptr comparisons (ptr == ptr is valid; ptr != ptr too)
  if ((Op == "==" || Op == "!=") &&
      LV->getType()->isPointerTy() && RV->getType()->isPointerTy()) {
    if (Op == "==") return Builder->CreateICmpEQ(LV, RV);
    return Builder->CreateICmpNE(LV, RV);
  }
  // Cast ptr to int for mixed ptr/int comparisons
  if ((Op == "==" || Op == "!=") &&
      LV->getType()->isPointerTy() && RV->getType()->isIntegerTy())
    LV = Builder->CreatePtrToInt(LV, RV->getType());
  if ((Op == "==" || Op == "!=") &&
      LV->getType()->isIntegerTy() && RV->getType()->isPointerTy())
    RV = Builder->CreatePtrToInt(RV, LV->getType());

  if (Op == "-")  return Builder->CreateSub(LV, RV);
  if (Op == "*")  return Builder->CreateMul(LV, RV);
  if (Op == "/")  return Builder->CreateSDiv(LV, RV);
  if (Op == "%")  return Builder->CreateSRem(LV, RV);
  if (Op == "==") return Builder->CreateICmpEQ(LV, RV);
  if (Op == "!=") return Builder->CreateICmpNE(LV, RV);
  if (Op == "<")  return Builder->CreateICmpSLT(LV, RV);
  if (Op == "<=") return Builder->CreateICmpSLE(LV, RV);
  if (Op == ">")  return Builder->CreateICmpSGT(LV, RV);
  if (Op == ">=") return Builder->CreateICmpSGE(LV, RV);
  if (Op == "&&") return Builder->CreateAnd(LV, RV);
  if (Op == "||") return Builder->CreateOr(LV, RV);
  if (Op == "&")  return Builder->CreateAnd(LV, RV);
  if (Op == "|")  return Builder->CreateOr(LV, RV);
  if (Op == "^")  return Builder->CreateXor(LV, RV);
  if (Op == "<<") return Builder->CreateShl(LV, RV);
  if (Op == ">>") return Builder->CreateAShr(LV, RV);

  return createStringError(inconvertibleErrorCode(), "unknown operator " + Op.str());
}

Expected<llvm::Value *> CodeGenerator::codegenArrayLiteral(ArrayLiteral *E) {
  // For now represent as an i32 count constant (Sema/runtime resolves the pointer)
  return ConstantInt::get(Builder->getInt32Ty(), (uint32_t)E->Elements.size());
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------
Error CodeGenerator::codegenBlock(BlockStmt *B) {
  for (auto &S : B->Statements) {
    // Stop emitting once the current block already has a terminator
    // (unreachable code after ret/br — would produce invalid IR).
    auto *CurBB = Builder->GetInsertBlock();
    if (CurBB && !CurBB->empty() && CurBB->back().isTerminator())
      break;
    if (auto E = codegenStmt(S.get())) return E;
  }
  return Error::success();
}

Error CodeGenerator::codegenStmt(ASTNode *S) {
  if (auto *VD = dyn_cast<VarDecl>(S))   return codegenVarDecl(VD);
  if (auto *IS = dyn_cast<IfStmt>(S))    return codegenIf(IS);
  if (auto *LS = dyn_cast<LoopStmt>(S))  return codegenLoop(LS);
  if (auto *LG = dyn_cast<LogStmt>(S))   return codegenLog(LG);
  if (auto *RS = dyn_cast<RetStmt>(S))   return codegenRet(RS);
  if (auto *AS = dyn_cast<AssignStmt>(S))return codegenAssign(AS);
  if (auto *ES = dyn_cast<ExprStmt>(S)) {
    auto V = codegenExpr(ES->E.get());
    if (!V) return V.takeError();
    return Error::success();
  }
  if (auto *FD = dyn_cast<FunctionDecl>(S)) {
    auto R = codegenFunction(FD);
    if (!R) return R.takeError();
    return Error::success();
  }
  // BreakStmt — branch to the enclosing loop's exit block.
  if (CurrentLoopExit) {
    auto *CurBB = Builder->GetInsertBlock();
    if (CurBB && (CurBB->empty() || !CurBB->back().isTerminator()))
      Builder->CreateBr(CurrentLoopExit);
  }
  return Error::success();
}

// Helper: cast Val to Ty if types differ (int<->ptr, int widening, ptr<->ptr).
static llvm::Value *castToType(IRBuilder<> *B, llvm::Value *Val, llvm::Type *Ty) {
  if (!Val || !Ty || Val->getType() == Ty) return Val;
  if (Ty->isPointerTy() && Val->getType()->isIntegerTy())
    return B->CreateIntToPtr(Val, Ty);
  if (Ty->isIntegerTy() && Val->getType()->isPointerTy())
    return B->CreatePtrToInt(Val, Ty);
  if (Ty->isIntegerTy() && Val->getType()->isIntegerTy())
    return B->CreateIntCast(Val, Ty, /*isSigned=*/true);
  if (Ty->isPointerTy() && Val->getType()->isPointerTy())
    return B->CreatePointerCast(Val, Ty);
  return Val;
}

Error CodeGenerator::codegenVarDecl(VarDecl *D) {
  llvm::Type *Ty = maraTypeToLLVM(D->TypeKind);

  if (ClassBodyScope) {
    // Class-level member variable: allocate as a module global so that nested
    // functions can read/write it without cross-function alloca references.
    // LLVM auto-renames if the name already exists (e.g. _ctx → _ctx.1),
    // preventing silent global-variable name collisions between classes.
    auto *GV = new GlobalVariable(*Mod, Ty, D->IsConst,
        GlobalValue::InternalLinkage, Constant::getNullValue(Ty), D->Name);
    // Track the actual LLVM name (post-deduplication) in the symbol table.
    Syms[GV->getName()] = GV;
    if (D->Initializer) {
      auto V = codegenExpr(D->Initializer.get());
      if (!V) return V.takeError();
      Builder->CreateStore(castToType(Builder.get(), *V, Ty), GV);
    }
    return Error::success();
  }

  llvm::AllocaInst *Alloca = Builder->CreateAlloca(Ty, nullptr, D->Name);

  if (D->Initializer) {
    auto V = codegenExpr(D->Initializer.get());
    if (!V) return V.takeError();
    Builder->CreateStore(castToType(Builder.get(), *V, Ty), Alloca);
  } else {
    Builder->CreateStore(Constant::getNullValue(Ty), Alloca);
  }

  Syms[D->Name] = Alloca;
  return Error::success();
}

Error CodeGenerator::codegenIf(IfStmt *S) {
  auto CondV = codegenExpr(S->Cond.get());
  if (!CondV) return CondV.takeError();

  llvm::Function *Fn = Builder->GetInsertBlock()->getParent();
  auto *ThenBB  = BasicBlock::Create(*Ctx, "then",  Fn);
  auto *MergeBB = BasicBlock::Create(*Ctx, "merge");
  // Only allocate ElseBB when there is an else clause — avoids orphaned BB leak.
  auto *ElseBB  = S->Else ? BasicBlock::Create(*Ctx, "else") : nullptr;

  Builder->CreateCondBr(*CondV, ThenBB, ElseBB ? ElseBB : MergeBB);

  Builder->SetInsertPoint(ThenBB);
  if (auto E = codegenBlock(S->Then.get())) return E;
  {
    auto *CurBB = Builder->GetInsertBlock();
    if (CurBB && (CurBB->empty() || !CurBB->back().isTerminator()))
      Builder->CreateBr(MergeBB);
  }

  if (S->Else && ElseBB) {
    Fn->insert(Fn->end(), ElseBB);
    Builder->SetInsertPoint(ElseBB);
    if (auto E = codegenBlock(S->Else.get())) return E;
    {
      auto *CurBB = Builder->GetInsertBlock();
      if (CurBB && (CurBB->empty() || !CurBB->back().isTerminator()))
        Builder->CreateBr(MergeBB);
    }
  }

  Fn->insert(Fn->end(), MergeBB);
  Builder->SetInsertPoint(MergeBB);
  return Error::success();
}

Error CodeGenerator::codegenLoop(LoopStmt *S) {
  llvm::Function *Fn = Builder->GetInsertBlock()->getParent();
  auto *CondBB = BasicBlock::Create(*Ctx, "loop.cond", Fn);
  auto *BodyBB = BasicBlock::Create(*Ctx, "loop.body");
  auto *ExitBB = BasicBlock::Create(*Ctx, "loop.exit");

  // Register ExitBB so that break statements can branch to it.
  auto *PrevLoopExit = CurrentLoopExit;
  CurrentLoopExit = ExitBB;

  Builder->CreateBr(CondBB);
  Builder->SetInsertPoint(CondBB);

  if (S->Cond) {
    auto CondV = codegenExpr(S->Cond.get());
    if (!CondV) return CondV.takeError();
    Builder->CreateCondBr(*CondV, BodyBB, ExitBB);
  } else {
    Builder->CreateBr(BodyBB); // infinite loop — break exits
  }

  Fn->insert(Fn->end(), BodyBB);
  Builder->SetInsertPoint(BodyBB);
  if (auto E = codegenBlock(S->Body.get())) return E;
  {
    auto *CurBB = Builder->GetInsertBlock();
    if (CurBB && (CurBB->empty() || !CurBB->back().isTerminator()))
      Builder->CreateBr(CondBB);
  }

  Fn->insert(Fn->end(), ExitBB);
  Builder->SetInsertPoint(ExitBB);
  CurrentLoopExit = PrevLoopExit; // restore outer loop exit (nested loops)
  return Error::success();
}

Error CodeGenerator::codegenLog(LogStmt *S) {
  auto Msg = codegenExpr(S->Message.get());
  if (!Msg) return Msg.takeError();

  // Cast to i8* if needed (for printf)
  llvm::Value *Ptr = Builder->CreatePointerCast(*Msg, Builder->getPtrTy());
  Builder->CreateCall(LogFunc, { Ptr });
  return Error::success();
}

Error CodeGenerator::codegenRet(RetStmt *S) {
  if (S->Value) {
    auto V = codegenExpr(S->Value.get());
    if (!V) return V.takeError();
    llvm::Value *RetVal = *V;
    // Cast return value to match the function's declared return type
    auto *Fn = Builder->GetInsertBlock()->getParent();
    llvm::Type *RetTy = Fn->getReturnType();
    RetVal = castToType(Builder.get(), RetVal, RetTy);
    Builder->CreateRet(RetVal);
  } else {
    Builder->CreateRetVoid();
  }
  return Error::success();
}

Error CodeGenerator::codegenAssign(AssignStmt *S) {
  auto RHS = codegenExpr(S->RHS.get());
  if (!RHS) return RHS.takeError();

  auto It = Syms.find(S->LHS);
  if (It == Syms.end())
    return createStringError(inconvertibleErrorCode(),
                             "undefined variable '" + S->LHS + "'");

  llvm::Type *DstTy = nullptr;
  if (auto *AI = dyn_cast<AllocaInst>(It->second))
    DstTy = AI->getAllocatedType();
  else if (auto *GV = dyn_cast<GlobalVariable>(It->second))
    DstTy = GV->getValueType();

  llvm::Value *Val = DstTy ? castToType(Builder.get(), *RHS, DstTy) : *RHS;
  Builder->CreateStore(Val, It->second);
  return Error::success();
}

// ---------------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------------
Expected<llvm::Function *> CodeGenerator::codegenFunction(FunctionDecl *F) {
  // Save outer context (insert point + symbol table)
  auto SavedSyms = Syms;
  auto *SavedBlock = Builder->GetInsertBlock();
  llvm::BasicBlock::iterator SavedPoint;
  if (SavedBlock) SavedPoint = Builder->GetInsertPoint();

  std::vector<llvm::Type *> ParamTypes;
  for (const auto &P : F->Params)
    ParamTypes.push_back(maraTypeToLLVM(P.TypeKind));

  auto *FT = llvm::FunctionType::get(Builder->getInt32Ty(), ParamTypes, false);
  auto Linkage = (FuncNestDepth == 0) ? GlobalValue::ExternalLinkage : visToLinkage(F->Vis);
  auto *Fn = llvm::Function::Create(FT, Linkage, F->Name, Mod.get());

  unsigned I = 0;
  for (auto &Arg : Fn->args())
    Arg.setName(F->Params[I++].Name);

  auto *Entry = BasicBlock::Create(*Ctx, "entry", Fn);
  Builder->SetInsertPoint(Entry);

  // Inherit outer scope so nested functions can see member variables
  Syms = SavedSyms;
  for (auto &Arg : Fn->args()) {
    auto *A = Builder->CreateAlloca(Arg.getType(), nullptr, Arg.getName());
    Builder->CreateStore(&Arg, A);
    Syms[Arg.getName()] = A;
  }

  // Detect class body: a rel that has nested rel declarations uses GlobalVariables
  // for its VarDecls so that nested functions can access them without cross-function
  // alloca references (which LLVM's verifier rejects).
  bool HasNestedFuncs = false;
  if (F->Body) {
    for (auto &S : F->Body->Statements)
      if (dyn_cast<FunctionDecl>(S.get())) { HasNestedFuncs = true; break; }
  }
  bool PrevClassBodyScope = ClassBodyScope;
  ClassBodyScope = HasNestedFuncs;
  FuncNestDepth++;

  if (F->Body) {
    if (auto E = codegenBlock(F->Body.get())) {
      ClassBodyScope = PrevClassBodyScope;
      FuncNestDepth--;
      return std::move(E);
    }
  }
  ClassBodyScope = PrevClassBodyScope;
  FuncNestDepth--;

  // Default return 0 if no proper terminator
  {
    auto *CurBB = Builder->GetInsertBlock();
    if (CurBB && (CurBB->empty() || !CurBB->back().isTerminator()))
      Builder->CreateRet(ConstantInt::get(Builder->getInt32Ty(), 0));
  }

  // Restore outer context
  Syms = SavedSyms;
  if (SavedBlock) Builder->SetInsertPoint(SavedBlock, SavedPoint);

  return Fn;
}

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------
Expected<std::unique_ptr<llvm::Module>> CodeGenerator::codegenModule(Module *M) {
  for (auto &GV : M->Globals) {
    auto *Ty = maraTypeToLLVM(GV->TypeKind);
    Constant *Init = GV->Initializer ? nullptr : Constant::getNullValue(Ty);
    (void)new GlobalVariable(*Mod, Ty, GV->IsConst,
                             GlobalValue::InternalLinkage, Init, GV->Name);
  }

  for (auto &F : M->Functions) {
    auto R = codegenFunction(F.get());
    if (!R) return R.takeError();
  }

  return std::move(Mod);
}

void CodeGenerator::optimize(llvm::Module &M) {
  PassBuilder PB;
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;

  PB.registerLoopAnalyses(LAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerModuleAnalyses(MAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(
      OptimizationLevel::O2);
  MPM.run(M, MAM);
}
