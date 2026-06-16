// Vyft Ltd — Maratine Compiler Driver — Proprietary — 2026
//
// maratine-cc: compiles *.mara source files to *.ovc (Vyft Compiled Output),
// LLVM IR (*.ll), or assembly (*.s) targeting Slura OS on ARM64.

#include "MaratineLexer.h"
#include "MaratineParser.h"
#include "MaratineSema.h"
#include "MaratineCodeGen.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::maratine;

static cl::opt<std::string> InputFile(
    cl::Positional,
    cl::desc("<input .mara file>"),
    cl::Required);

static cl::opt<std::string> OutputFile(
    "o",
    cl::desc("Output filename (*.ovc, *.ll, *.s)"),
    cl::value_desc("filename"));

static cl::opt<std::string> OutputFormat(
    "emit",
    cl::init("ovc"),
    cl::desc("Output format: ovc (app), llvm (LLVM IR), asm"),
    cl::value_desc("format"));

static cl::opt<bool> DumpAST(
    "dump-ast",
    cl::desc("Dump abstract syntax tree"));

static cl::opt<bool> DumpTokens(
    "dump-tokens",
    cl::desc("Dump lexer tokens"));

static cl::opt<bool> Optimize(
    "O",
    cl::desc("Enable O2 optimizations"));

int main(int argc, char **argv) {
  cl::SetVersionPrinter([](raw_ostream &OS) {
    OS << "maratine-cc v0.1 Naverta build 26160621 beta\n";
  });
  cl::ParseCommandLineOptions(argc, argv,
      "maratine-cc - Maratine Compiler (Slura OS / ARM64 + X64)\n");

  auto BufferOrErr = MemoryBuffer::getFile(InputFile);
  if (!BufferOrErr) {
    errs() << "Error reading source file: "
           << BufferOrErr.getError().message() << "\n";
    return 1;
  }

  StringRef Source = BufferOrErr.get()->getBuffer();
  errs() << "=== maratine-cc ===\n";
  errs() << "Source: " << InputFile << "  (" << Source.count('\n') << " lines)\n\n";

  // --- 1. Lexical analysis ---
  errs() << "[1/4] Lexical analysis...\n";
  Lexer L(Source);
  auto Tokens = L.tokenize();

  if (DumpTokens) {
    errs() << Tokens.size() << " tokens:\n";
    for (const auto &T : Tokens)
      errs() << "  [" << T.Line << ":" << T.Column << "] "
             << (int)T.Kind << " = " << T.Value << "\n";
  }

  // --- 2. Parsing ---
  errs() << "[2/4] Parsing...\n";
  Parser P(Tokens);
  auto ModOrErr = P.parseModule();
  if (!ModOrErr) {
    errs() << "Parse error: " << toString(ModOrErr.takeError()) << "\n";
    return 1;
  }
  auto &Mod = *ModOrErr;

  if (DumpAST)
    P.printAST(*Mod);

  // --- 3. Semantic analysis ---
  errs() << "[3/4] Semantic analysis...\n";
  Sema S;
  if (auto Err = S.analyse(*Mod)) {
    errs() << "Semantic error:\n" << toString(std::move(Err)) << "\n";
    return 1;
  }
  if (!S.diagnostics().empty())
    S.printDiagnostics(errs());

  // --- 4. Code generation ---
  errs() << "[4/5] Code generation (ARM64)...\n";
  CodeGenerator CG("maratine_module");
  auto LLVMModOrErr = CG.codegenModule(Mod.get());
  if (!LLVMModOrErr) {
    errs() << "Codegen error: " << toString(LLVMModOrErr.takeError()) << "\n";
    return 1;
  }
  auto LLVMMod = std::move(*LLVMModOrErr);

  // --- 5. Optimization ---
  if (Optimize) {
    errs() << "[5/5] Optimization (O2)...\n";
    CG.optimize();
  } else {
    errs() << "[5/5] Skipping optimization (pass -O to enable)\n";
  }

  // --- Output ---
  std::string OutFile = OutputFile;
  if (OutFile.empty()) {
    StringRef Base = StringRef(InputFile).rsplit('.').first;
    if (OutputFormat == "ovc")
      OutFile = (Base + ".ovc").str();
    else if (OutputFormat == "llvm")
      OutFile = (Base + ".ll").str();
    else
      OutFile = (Base + ".s").str();
  }

  std::error_code EC;
  ToolOutputFile Out(OutFile, EC, sys::fs::OF_None);
  if (EC) {
    errs() << "Cannot open output: " << EC.message() << "\n";
    return 1;
  }

  if (OutputFormat == "ovc") {
    Out.os() << "# Vyft OVC v1.0 — Maratine Compiled Application\n";
    Out.os() << "# Target: Slura OS / ARM64\n\n";
    LLVMMod->print(Out.os(), nullptr);
  } else if (OutputFormat == "llvm") {
    LLVMMod->print(Out.os(), nullptr);
  } else if (OutputFormat == "asm") {
    errs() << "ASM output: not yet implemented\n";
    return 1;
  } else {
    errs() << "Unknown output format: " << OutputFormat << "\n";
    return 1;
  }

  Out.keep();
  errs() << "Compilation successful: " << OutFile << "\n";
  return 0;
}
