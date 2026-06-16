//===-- MaraBug.cpp – Mara/Maratine Debugger ─────────────────────────────===//
//
// MaraBug — Débogueur interactif pour Mara/Maratine
// Auteur : Vyft Ltd — 2026
// Licence : Proprietary
//
// Usage :
//   marabug <executable.marep> [-- args...]
//   marabug --pid <PID>
//
// Commandes REPL :
//   run / r                   Lancer / reprendre l'exécution
//   break / b <file:line>     Breakpoint sur une ligne
//   break / b <FuncName>      Breakpoint sur une fonction Mara
//   delete / d <id>           Supprimer un breakpoint
//   enable / disable <id>     Activer/désactiver un breakpoint
//   breakpoints / bl          Lister les breakpoints
//   step / s                  Pas-à-pas dans la fonction (step into)
//   next / n                  Pas-à-pas ligne (step over)
//   finish / f                Sortir de la fonction (step out)
//   backtrace / bt            Afficher la pile d'appel
//   frame / fr <n>            Sélectionner un frame
//   locals / lv               Variables locales du frame courant
//   print / p <expr>          Évaluer une expression Mara
//   source / src              Afficher le source courant
//   list / l [file:line]      Afficher le source à une position
//   watch <var>               Watchpoint sur une variable
//   abi                       Afficher les infos ABI MABI 1.0.1
//   quit / q                  Quitter MaraBug
//   help / h                  Afficher cette aide
//
//===----------------------------------------------------------------------===//

#include "MaraBugSession.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/StringRef.h"
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace llvm;
using namespace marabug;

//----------------------------------------------------------------------
// Options CLI
//----------------------------------------------------------------------

static cl::opt<std::string> ExecPath(cl::Positional,
  cl::desc("<executable.marep>"), cl::Optional);

static cl::opt<unsigned> AttachPID("pid",
  cl::desc("PID du processus à attacher"), cl::init(0));

static cl::list<std::string> ExecArgs("--",
  cl::desc("Arguments passés à l'exécutable"), cl::ZeroOrMore);

//----------------------------------------------------------------------
// Affichage
//----------------------------------------------------------------------

static void printBanner(raw_ostream &OS) {
  OS << "\n";
  WithColor(OS, raw_ostream::MAGENTA, true)
    << "╔══════════════════════════════════════════╗\n"
       "║  MaraBug — Mara/Maratine Debugger 1.0   ║\n"
       "║  MABI 1.0.1  ·  ARM64  ·  Vyft Ltd      ║\n"
       "╚══════════════════════════════════════════╝\n\n";
  OS << "Tape 'help' pour la liste des commandes.\n\n";
}

static void printHelp(raw_ostream &OS) {
  OS << "\n";
  WithColor(OS, raw_ostream::CYAN, true) << "Commandes MaraBug :\n";
  OS << "  run / r                  Lancer ou reprendre l'exécution\n";
  OS << "  break / b <file:line>    Breakpoint sur une ligne .mara\n";
  OS << "  break / b <Func>         Breakpoint sur une fonction Mara\n";
  OS << "  delete / d <id>          Supprimer un breakpoint\n";
  OS << "  enable <id>              Activer un breakpoint\n";
  OS << "  disable <id>             Désactiver un breakpoint\n";
  OS << "  breakpoints / bl         Lister tous les breakpoints\n";
  OS << "  step / s                 Step into (entre dans la fonction)\n";
  OS << "  next / n                 Step over (pas-à-pas ligne)\n";
  OS << "  finish / f               Step out (sortir de la fonction)\n";
  OS << "  backtrace / bt           Pile d'appel Mara complète\n";
  OS << "  frame / fr <n>           Sélectionner un frame\n";
  OS << "  locals / lv              Variables locales du frame courant\n";
  OS << "  print / p <expr>         Évaluer une expression Mara\n";
  OS << "  source / src             Source .mara autour de la position\n";
  OS << "  list / l [file:line]     Afficher le source à une position\n";
  OS << "  watch <var>              Watchpoint sur une variable\n";
  OS << "  abi                      Infos ABI MABI 1.0.1 du binaire\n";
  OS << "  quit / q                 Quitter MaraBug\n";
  OS << "  help / h                 Cette aide\n\n";
}

static void printBreakpoints(const DebugSession &S, raw_ostream &OS) {
  const auto &bps = S.breakpoints();
  if (bps.empty()) {
    OS << "  Aucun breakpoint défini.\n";
    return;
  }
  OS << "\n";
  for (const auto &B : bps) {
    StringRef state = B.Enabled ? "\033[32m●\033[0m" : "\033[90m○\033[0m";
    OS << "  [" << B.ID << "] " << state << "  ";
    if (!B.Function.empty())
      OS << "fn " << B.Function;
    else
      OS << B.File << ":" << B.Line;
    OS << "  (hits: " << B.HitCount << ")\n";
  }
  OS << "\n";
}

static void printBacktrace(const DebugSession &S, raw_ostream &OS) {
  auto frames = S.backtrace();
  if (frames.empty()) { OS << "  Pile d'appel vide.\n"; return; }
  OS << "\n";
  for (const auto &Fr : frames) {
    OS << "  #" << Fr.Index << "  ";
    WithColor(OS, raw_ostream::CYAN) << Fr.Function;
    OS << "  " << Fr.File << ":" << Fr.Line
       << "  [0x" << llvm::format_hex(Fr.PC, 18) << "]\n";
  }
  OS << "\n";
}

static void printLocals(const DebugSession &S, unsigned Frame,
                         raw_ostream &OS) {
  auto vars = S.locals(Frame);
  if (vars.empty()) { OS << "  Aucune variable locale.\n"; return; }
  OS << "\n";
  for (const auto &V : vars) {
    StringRef mod = V.IsConst ? "let" : "var";
    OS << "  " << mod << " " << V.Name << ": " << V.Type
       << " = " << V.Value << "\n";
  }
  OS << "\n";
}

//----------------------------------------------------------------------
// Parseur de commandes REPL
//----------------------------------------------------------------------

static std::vector<std::string> tokenize(const std::string &Line) {
  std::vector<std::string> tokens;
  std::istringstream iss(Line);
  std::string tok;
  while (iss >> tok) tokens.push_back(tok);
  return tokens;
}

//----------------------------------------------------------------------
// Boucle REPL
//----------------------------------------------------------------------

static int repl(DebugSession &Session) {
  raw_ostream &OS = outs();
  unsigned currentFrame = 0;

  while (true) {
    // Prompt
    WithColor(OS, raw_ostream::GREEN, true) << "(marabug) ";
    OS.flush();

    std::string line;
    if (!std::getline(std::cin, line)) break;  // EOF = quit
    if (line.empty()) continue;

    auto toks = tokenize(line);
    if (toks.empty()) continue;
    StringRef cmd = toks[0];

    // ── run / r ──────────────────────────────────────────────────────
    if (cmd == "run" || cmd == "r") {
      if (Session.state() == DebugSession::State::Detached) {
        WithColor::error() << "Aucun exécutable chargé.\n";
      } else {
        Session.continueExec(OS);
      }

    // ── break / b ────────────────────────────────────────────────────
    } else if (cmd == "break" || cmd == "b") {
      if (toks.size() < 2) {
        WithColor::warning() << "Usage: break <file:line> | <FunctionName>\n";
        continue;
      }
      StringRef spec = toks[1];
      if (spec.contains(':')) {
        auto [file, linePart] = spec.split(':');
        unsigned ln = 0;
        linePart.getAsInteger(10, ln);
        auto &BP = Session.addBreakpoint(file, ln);
        OS << "  Breakpoint #" << BP.ID << " : " << file << ":" << ln << "\n";
      } else {
        auto &BP = Session.addFunctionBreakpoint(spec);
        OS << "  Breakpoint #" << BP.ID << " : fn " << spec << "\n";
      }

    // ── delete / d ───────────────────────────────────────────────────
    } else if (cmd == "delete" || cmd == "d") {
      if (toks.size() < 2) { OS << "Usage: delete <id>\n"; continue; }
      unsigned id = 0;
      StringRef(toks[1]).getAsInteger(10, id);
      Session.removeBreakpoint(id)
        ? OS << "  Breakpoint #" << id << " supprimé.\n"
        : WithColor::error() << "Breakpoint #" << id << " introuvable.\n";

    // ── enable / disable ─────────────────────────────────────────────
    } else if (cmd == "enable" || cmd == "disable") {
      if (toks.size() < 2) continue;
      unsigned id = 0; StringRef(toks[1]).getAsInteger(10, id);
      Session.setBreakpointEnabled(id, cmd == "enable");
      OS << "  Breakpoint #" << id << (cmd == "enable" ? " activé" : " désactivé") << ".\n";

    // ── breakpoints / bl ─────────────────────────────────────────────
    } else if (cmd == "breakpoints" || cmd == "bl") {
      printBreakpoints(Session, OS);

    // ── step / s ─────────────────────────────────────────────────────
    } else if (cmd == "step" || cmd == "s") {
      Session.stepInto(OS);

    // ── next / n ─────────────────────────────────────────────────────
    } else if (cmd == "next" || cmd == "n") {
      Session.stepOver(OS);
      Session.showSource(OS, 3);

    // ── finish / f ───────────────────────────────────────────────────
    } else if (cmd == "finish" || cmd == "f") {
      Session.stepOut(OS);

    // ── backtrace / bt ───────────────────────────────────────────────
    } else if (cmd == "backtrace" || cmd == "bt") {
      printBacktrace(Session, OS);

    // ── frame / fr ───────────────────────────────────────────────────
    } else if (cmd == "frame" || cmd == "fr") {
      if (toks.size() >= 2) {
        StringRef(toks[1]).getAsInteger(10, currentFrame);
        OS << "  Frame #" << currentFrame << " sélectionné.\n";
      }

    // ── locals / lv ──────────────────────────────────────────────────
    } else if (cmd == "locals" || cmd == "lv") {
      printLocals(Session, currentFrame, OS);

    // ── print / p ────────────────────────────────────────────────────
    } else if (cmd == "print" || cmd == "p") {
      if (toks.size() < 2) { OS << "Usage: print <expr>\n"; continue; }
      std::string expr = line.substr(cmd.size() + 1);
      std::string result = Session.evaluate(expr);
      OS << "  = " << result << "\n";

    // ── source / src ─────────────────────────────────────────────────
    } else if (cmd == "source" || cmd == "src") {
      Session.showSource(OS, 5);

    // ── abi ──────────────────────────────────────────────────────────
    } else if (cmd == "abi") {
      WithColor(OS, raw_ostream::CYAN, true) << "MABI 1.0.1\n";
      OS << "  Calling convention : AAPCS64\n";
      OS << "  Mangling           : _Mara_<Module>_<Symbol>_<Hash6>\n";
      OS << "  rel op             → ExternalLinkage\n";
      OS << "  rel cl             → InternalLinkage\n";

    // ── help / h ─────────────────────────────────────────────────────
    } else if (cmd == "help" || cmd == "h") {
      printHelp(OS);

    // ── quit / q ─────────────────────────────────────────────────────
    } else if (cmd == "quit" || cmd == "q") {
      Session.detach();
      OS << "MaraBug : session terminée.\n\n";
      return 0;

    } else {
      WithColor::warning() << "Commande inconnue : '" << cmd
                           << "'. Tape 'help' pour l'aide.\n";
    }
  }
  return 0;
}

//----------------------------------------------------------------------
// main
//----------------------------------------------------------------------

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::SetVersionPrinter([](raw_ostream &OS) {
    OS << "marabug v0.1 Naverta build 26160621 beta\n";
  });
  cl::ParseCommandLineOptions(argc, argv,
    "marabug - MaraBug Mara/Maratine Debugger (MABI 1.0.1)\n");

  raw_ostream &OS = outs();
  printBanner(OS);

  DebugSession session;

  if (AttachPID > 0) {
    OS << "Attachement au PID " << AttachPID << "…\n";
    if (!session.attach(AttachPID, OS)) {
      WithColor::error() << "Impossible d'attacher au PID " << AttachPID << ".\n";
      return 1;
    }
    OS << "Attaché. Tape 'run' pour continuer.\n\n";

  } else if (!ExecPath.empty()) {
    OS << "Chargement : " << ExecPath << "\n";
    std::vector<std::string> args(ExecArgs.begin(), ExecArgs.end());
    if (!session.launch(ExecPath, args, OS)) {
      WithColor::error() << "Impossible de lancer : " << ExecPath << ".\n";
      return 1;
    }
    OS << "Prêt. Tape 'run' pour démarrer.\n\n";

  } else {
    OS << "Usage : marabug <executable.marep> [-- args]\n";
    OS << "        marabug --pid <PID>\n\n";
    OS << "Entrez en mode interactif (pas d'exécutable chargé).\n\n";
  }

  return repl(session);
}
