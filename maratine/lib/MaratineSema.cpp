// Vyft Ltd — Mara/Maratine Semantic Analysis — Proprietary — 2026

#include "MaratineSema.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::maratine;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Error Sema::analyse(Module &M) {
  Ctx.push(); // global scope

  analyseGlobals(M);
  registerFunctionSignatures(M);

  for (auto &F : M.Functions)
    analyseFunction(*F);

  Ctx.pop();

  if (hasErrors()) {
    std::string Buf;
    raw_string_ostream OS(Buf);
    printDiagnostics(OS);
    return createStringError(inconvertibleErrorCode(), OS.str());
  }
  return Error::success();
}

bool Sema::hasErrors() const {
  for (auto &D : Diags)
    if (D.IsErr) return true;
  return false;
}

void Sema::printDiagnostics(raw_ostream &OS) const {
  for (auto &D : Diags) {
    OS << (D.IsErr ? "error" : "warning");
    if (D.Line) OS << " [" << D.Line << ":" << D.Col << "]";
    OS << ": " << D.Msg << "\n";
  }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void Sema::diag(unsigned L, unsigned C, bool Err, StringRef Msg) {
  Diags.push_back({L, C, Err, Msg.str()});
}

bool Sema::typesCompatible(MaraTypeKind Dst, MaraTypeKind Src) const {
  if (Dst == MaraTypeKind::Unknown || Src == MaraTypeKind::Unknown) return true;
  if (Dst == Src) return true;
  // i32 / i64 / u64 are mutually assignable (implicit widening)
  auto isInt = [](MaraTypeKind K) {
    return K == MaraTypeKind::I32 || K == MaraTypeKind::I64 ||
           K == MaraTypeKind::U64;
  };
  if (isInt(Dst) && isInt(Src)) return true;
  // ptr / string / array / compound are pointer-like — allow cross-assignment
  auto isPtr = [](MaraTypeKind K) {
    return K == MaraTypeKind::Ptr || K == MaraTypeKind::String ||
           K == MaraTypeKind::Array || K == MaraTypeKind::Compound;
  };
  if (isPtr(Dst) && isPtr(Src)) return true;
  return false;
}

std::string Sema::typeStr(MaraTypeKind K) const {
  switch (K) {
    case MaraTypeKind::String:   return "string";
    case MaraTypeKind::I32:      return "i32";
    case MaraTypeKind::I64:      return "i64";
    case MaraTypeKind::U64:      return "u64";
    case MaraTypeKind::Bool:     return "bool";
    case MaraTypeKind::Ptr:      return "ptr";
    case MaraTypeKind::Array:    return "array";
    case MaraTypeKind::Compound: return "compound";
    default:                     return "unknown";
  }
}

// ---------------------------------------------------------------------------
// Global analysis
// ---------------------------------------------------------------------------

void Sema::analyseGlobals(Module &M) {
  for (auto &G : M.Globals) {
    SymbolEntry S;
    S.Name        = G->Name;
    S.TypeKind    = G->TypeKind;
    S.CompoundName = G->CompoundTypeName;
    S.IsConst     = G->IsConst;
    if (!Ctx.define(S))
      err(0, 0, "redefinition of global '" + G->Name + "'");

    if (G->Initializer) {
      MaraTypeKind InitTy = analyseExpr(*G->Initializer);
      if (!typesCompatible(G->TypeKind, InitTy))
        err(0, 0, "type mismatch in initializer of '" + G->Name + "': expected " +
            typeStr(G->TypeKind) + ", got " + typeStr(InitTy));
    }
  }
}

void Sema::registerFunctionSignatures(Module &M) {
  for (auto &F : M.Functions) {
    SymbolEntry S;
    S.Name       = F->Name;
    S.TypeKind   = MaraTypeKind::Unknown; // return type not yet inferred
    S.IsFunction = true;
    S.ParamCount = (unsigned)F->Params.size();
    Ctx.define(S); // duplicates allowed at global scope for overloads — warn only
  }
}

// ---------------------------------------------------------------------------
// Function analysis
// ---------------------------------------------------------------------------

void Sema::analyseFunction(FunctionDecl &F) {
  InFunction  = true;
  SeenRet     = false;
  ReturnType  = MaraTypeKind::Unknown;

  Ctx.push(); // function scope

  // Define parameters
  for (auto &P : F.Params) {
    SymbolEntry S;
    S.Name        = P.Name;
    S.TypeKind    = P.TypeKind;
    S.CompoundName = P.CompoundTypeName;
    if (!Ctx.define(S))
      err(0, 0, "duplicate parameter '" + P.Name + "' in function '" + F.Name + "'");
  }

  if (F.Body)
    analyseBlock(*F.Body);

  Ctx.pop();
  InFunction = false;
}

// ---------------------------------------------------------------------------
// Block / statement analysis
// ---------------------------------------------------------------------------

void Sema::analyseBlock(BlockStmt &B) {
  Ctx.push();
  for (auto &S : B.Statements)
    analyseStmt(*S);
  Ctx.pop();
}

void Sema::analyseStmt(ASTNode &S) {
  switch (S.getKind()) {
    case ASTNode::NK_VarDecl:    analyseVarDecl(cast<VarDecl>(S));       break;
    case ASTNode::NK_IfStmt:     analyseIfStmt(cast<IfStmt>(S));         break;
    case ASTNode::NK_LoopStmt:   analyseLoopStmt(cast<LoopStmt>(S));     break;
    case ASTNode::NK_RetStmt:    analyseRetStmt(cast<RetStmt>(S));        break;
    case ASTNode::NK_AssignStmt: analyseAssignStmt(cast<AssignStmt>(S)); break;
    case ASTNode::NK_LogStmt:    analyseLogStmt(cast<LogStmt>(S));        break;
    case ASTNode::NK_ExprStmt:   analyseExprStmt(cast<ExprStmt>(S));     break;
    case ASTNode::NK_FunctionDecl:
      analyseFunction(cast<FunctionDecl>(S)); // nested rel
      break;
    case ASTNode::NK_BreakStmt:
      if (!InFunction)
        err(0, 0, "break outside loop");
      break;
    default: break;
  }
}

void Sema::analyseVarDecl(VarDecl &D) {
  SymbolEntry S;
  S.Name        = D.Name;
  S.TypeKind    = D.TypeKind;
  S.CompoundName = D.CompoundTypeName;
  S.IsConst     = D.IsConst;

  if (!Ctx.define(S))
    err(0, 0, "redefinition of '" + D.Name + "'");

  if (D.Initializer) {
    MaraTypeKind InitTy = analyseExpr(*D.Initializer);
    if (!typesCompatible(D.TypeKind, InitTy))
      err(0, 0, "type mismatch initializing '" + D.Name + "': expected " +
          typeStr(D.TypeKind) + ", got " + typeStr(InitTy));
  }
}

void Sema::analyseIfStmt(IfStmt &S) {
  if (S.Cond) analyseExpr(*S.Cond);
  if (S.Then) analyseBlock(*S.Then);
  if (S.Else) analyseBlock(*S.Else);
}

void Sema::analyseLoopStmt(LoopStmt &S) {
  if (S.Cond) {
    MaraTypeKind CT = analyseExpr(*S.Cond);
    if (CT != MaraTypeKind::Bool && CT != MaraTypeKind::I32 &&
        CT != MaraTypeKind::Unknown)
      warn(0, 0, "loop condition is not boolean or i32");
  }
  if (S.Body) analyseBlock(*S.Body);
}

void Sema::analyseRetStmt(RetStmt &S) {
  SeenRet = true;
  if (S.Value) analyseExpr(*S.Value);
}

void Sema::analyseAssignStmt(AssignStmt &S) {
  const SymbolEntry *Sym = Ctx.lookup(S.LHS);
  if (!Sym) {
    err(0, 0, "assignment to undeclared variable '" + S.LHS + "'");
    if (S.RHS) analyseExpr(*S.RHS);
    return;
  }
  if (Sym->IsConst)
    err(0, 0, "assignment to constant '" + S.LHS + "'");

  if (S.RHS) {
    MaraTypeKind RhsTy = analyseExpr(*S.RHS);
    if (!typesCompatible(Sym->TypeKind, RhsTy))
      err(0, 0, "type mismatch assigning to '" + S.LHS + "': expected " +
          typeStr(Sym->TypeKind) + ", got " + typeStr(RhsTy));
  }
}

void Sema::analyseLogStmt(LogStmt &S) {
  if (S.Message) analyseExpr(*S.Message);
}

void Sema::analyseExprStmt(ExprStmt &S) {
  if (S.E) analyseExpr(*S.E);
}

// ---------------------------------------------------------------------------
// Expression analysis — returns inferred type
// ---------------------------------------------------------------------------

MaraTypeKind Sema::analyseExpr(Expr &E) {
  switch (E.getKind()) {
    case Expr::EK_StringLiteral: return MaraTypeKind::String;
    case Expr::EK_BoolLiteral:   return MaraTypeKind::Bool;
    case Expr::EK_NullLiteral:   return MaraTypeKind::Ptr;
    case Expr::EK_ArrayLiteral:  return MaraTypeKind::Array;
    case Expr::EK_IntLiteral:    return cast<IntLiteral>(E).IntTyKind;
    case Expr::EK_VarRef:        return analyseVarRef(cast<VarRef>(E));
    case Expr::EK_FFICallExpr:   return analyseFFICall(cast<FFICallExpr>(E));
    case Expr::EK_CallExpr:      return analyseCallExpr(cast<CallExpr>(E));
    case Expr::EK_BinaryExpr:    return analyseBinaryExpr(cast<BinaryExpr>(E));
    default:                     return MaraTypeKind::Unknown;
  }
}

MaraTypeKind Sema::analyseVarRef(VarRef &E) {
  const SymbolEntry *S = Ctx.lookup(E.Name);
  if (!S) {
    err(0, 0, "use of undeclared identifier '" + E.Name + "'");
    return MaraTypeKind::Unknown;
  }
  return S->TypeKind;
}

MaraTypeKind Sema::analyseFFICall(FFICallExpr &E) {
  // FFI calls are resolved at link time — analyse args, return i32 (convention)
  for (auto &A : E.Args)
    analyseExpr(*A);
  return MaraTypeKind::I32;
}

MaraTypeKind Sema::analyseCallExpr(CallExpr &E) {
  const SymbolEntry *S = Ctx.lookup(E.FunctionName);
  if (!S) {
    err(0, 0, "call to undeclared function '" + E.FunctionName + "'");
  } else if (S->IsFunction && S->ParamCount != E.Args.size()) {
    err(0, 0, "function '" + E.FunctionName + "' expects " +
        std::to_string(S->ParamCount) + " arguments, got " +
        std::to_string(E.Args.size()));
  }
  for (auto &A : E.Args)
    analyseExpr(*A);
  return MaraTypeKind::Unknown; // return type inference not yet implemented
}

MaraTypeKind Sema::analyseBinaryExpr(BinaryExpr &E) {
  MaraTypeKind L = analyseExpr(*E.LHS);
  MaraTypeKind R = analyseExpr(*E.RHS);

  // Comparison operators always produce bool
  static const char *CmpOps[] = {"==", "!=", "<", "<=", ">", ">="};
  for (auto *Op : CmpOps)
    if (E.Op == Op) return MaraTypeKind::Bool;

  // Logical operators require bool-ish operands
  if (E.Op == "&&" || E.Op == "||") {
    if (L != MaraTypeKind::Bool && L != MaraTypeKind::I32 &&
        L != MaraTypeKind::Unknown)
      warn(0, 0, "left operand of '" + E.Op + "' is not boolean");
    return MaraTypeKind::Bool;
  }

  // Arithmetic — string concatenation via + is allowed
  if (E.Op == "+") {
    if (L == MaraTypeKind::String || R == MaraTypeKind::String)
      return MaraTypeKind::String;
  }

  // Promote to widest integer type
  if (L == MaraTypeKind::I64 || R == MaraTypeKind::I64) return MaraTypeKind::I64;
  if (L == MaraTypeKind::U64 || R == MaraTypeKind::U64) return MaraTypeKind::U64;
  if (L == MaraTypeKind::I32 || R == MaraTypeKind::I32) return MaraTypeKind::I32;

  return L != MaraTypeKind::Unknown ? L : R;
}
