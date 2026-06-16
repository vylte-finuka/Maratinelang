//===-- Marai.cpp – Maratine Package Manager ─────────────────────────────===//
//
// marai — Maratine Package Manager
// Auteur : Vyft Ltd — 2026
// Licence : Proprietary
//
// Usage :
//   marai install <pkg[@ver]> [--force] [--dry-run] [--verbose]
//   marai update
//   marai remove <pkg>
//   marai list [--deps]
//   marai audit [--json] [--min-severity LEVEL]
//   marai audit --aude [--sbom] [--json]
//   marai publish <path>
//   marai abi check <pkg>
//   marai version
//
//===----------------------------------------------------------------------===//

#include "MaraiABI.h"
#include "MaraiAudit.h"
#include "MaraiInstall.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include <string>
#include <vector>

using namespace llvm;
using namespace marai;

//----------------------------------------------------------------------
// Définition des options CLI
//----------------------------------------------------------------------

static cl::SubCommand InstallCmd ("install",  "Installer un ou plusieurs packages");
static cl::SubCommand UpdateCmd  ("update",   "Mettre à jour tous les packages");
static cl::SubCommand RemoveCmd  ("remove",   "Supprimer un package");
static cl::SubCommand ListCmd    ("list",     "Lister les packages installés");
static cl::SubCommand AuditCmd   ("audit",    "Analyser les failles de sécurité");
static cl::SubCommand PublishCmd ("publish",  "Publier un package sur le registry");
static cl::SubCommand ABICmd     ("abi",      "Opérations ABI MABI 1.0.1");
static cl::SubCommand VersionCmd ("version",  "Afficher la version de marai");

// -- install
static cl::list<std::string> InstallSpecs(cl::Positional,
  cl::desc("<package[@version]>..."), cl::sub(InstallCmd));
static cl::opt<bool> Force("force",
  cl::desc("Forcer l'installation (ignore conflits ABI)"), cl::sub(InstallCmd));
static cl::opt<bool> DryRun("dry-run",
  cl::desc("Simuler sans installer"), cl::sub(InstallCmd));
static cl::opt<bool> Verbose("verbose",
  cl::desc("Sortie détaillée"), cl::sub(InstallCmd));
static cl::opt<bool> GlobalScope("global",
  cl::desc("Installation globale"), cl::sub(InstallCmd));
static cl::opt<std::string> Registry("registry",
  cl::desc("URL du registry"), cl::init("https://registry.maratine.dev"),
  cl::sub(InstallCmd));

// -- remove
static cl::opt<std::string> RemovePkg(cl::Positional,
  cl::desc("<package>"), cl::sub(RemoveCmd));

// -- list
static cl::opt<bool> ListDeps("deps",
  cl::desc("Afficher les dépendances"), cl::sub(ListCmd));

// -- audit
static cl::opt<bool> AudeMode("aude",
  cl::desc("Audit cybersec avancé niveau ingénieur"), cl::sub(AuditCmd));
static cl::opt<bool> AuditJSON("json",
  cl::desc("Sortie JSON"), cl::sub(AuditCmd));
static cl::opt<bool> AuditSBOM("sbom",
  cl::desc("Exporter le SBOM complet (avec --aude)"), cl::sub(AuditCmd));
static cl::opt<std::string> MinSeverity("min-severity",
  cl::desc("Niveau minimum : INFO LOW MEDIUM HIGH CRITICAL"),
  cl::init("LOW"), cl::sub(AuditCmd));
static cl::opt<std::string> LockfilePath("lockfile",
  cl::desc("Chemin du Maralock.yaml"), cl::init("./Maralock.yaml"),
  cl::sub(AuditCmd));

// -- publish
static cl::opt<std::string> PublishPath(cl::Positional,
  cl::desc("<package-path>"), cl::sub(PublishCmd));

// -- abi
static cl::opt<std::string> ABISubCmd(cl::Positional,
  cl::desc("check"), cl::sub(ABICmd));
static cl::opt<std::string> ABIPkg(cl::Positional,
  cl::desc("<package>"), cl::sub(ABICmd));

//----------------------------------------------------------------------
// Aide contextuelle
//----------------------------------------------------------------------

static void printHeader(raw_ostream &OS) {
  OS << "\n";
  WithColor(OS, raw_ostream::MAGENTA, true)
    << "marai — Maratine Package Manager\n";
  OS << "MABI 1.0.1  ·  Vyft Ltd  ·  2026\n\n";
}

static void printVersion(raw_ostream &OS) {
  printHeader(OS);
  OS << "marai     1.0.0\n";
  OS << "MABI      1.0.1\n";
  OS << "Registry  https://registry.maratine.dev\n\n";
}

//----------------------------------------------------------------------
// Commandes
//----------------------------------------------------------------------

static int cmdInstall(raw_ostream &OS) {
  if (InstallSpecs.empty()) {
    WithColor::error() << "marai install : aucun package spécifié.\n";
    OS << "Usage: marai install <pkg[@ver]>... [--force] [--dry-run]\n";
    return 1;
  }

  InstallOptions Opts;
  Opts.Force       = Force;
  Opts.DryRun      = DryRun;
  Opts.Verbose     = Verbose;
  Opts.GlobalScope = GlobalScope;
  Opts.RegistryURL = Registry;

  if (Force)
    WithColor(OS, raw_ostream::YELLOW)
      << "⚠  --force activé : les conflits ABI seront ignorés.\n\n";

  Installer installer(Opts);
  auto results = installer.install(
    std::vector<std::string>(InstallSpecs.begin(), InstallSpecs.end()));

  bool hasError = false;
  for (const auto &R : results) {
    R.print(OS, true);
    if (!R.success()) hasError = true;
  }
  return hasError ? 1 : 0;
}

static int cmdAudit(raw_ostream &OS) {
  bool color = true;

  if (AudeMode) {
    WithColor(OS, raw_ostream::MAGENTA, true)
      << "Démarrage de l'audit cybersec avancé --aude…\n\n";

    AudeReport report = AuditEngine::runAude(LockfilePath, ".", color);

    if (AuditJSON)
      report.printJSON(OS);
    else {
      report.printAude(OS, color);
      if (AuditSBOM) report.printAudeSBOM(OS);
    }

    return report.hasBlockers() ? 2 : 0;
  }

  // Audit standard
  AuditReport report = AuditEngine::run(LockfilePath, color);

  if (AuditJSON)
    report.printJSON(OS);
  else
    report.print(OS, color);

  return report.hasBlockers() ? 2 : 0;
}

static int cmdABI(raw_ostream &OS) {
  if (ABISubCmd == "check") {
    if (ABIPkg.empty()) {
      WithColor::error() << "marai abi check : package manquant.\n";
      return 1;
    }
    WithColor(OS, raw_ostream::CYAN, true)
      << "Vérification MABI 1.0.1 pour : " << ABIPkg << "\n\n";

    auto result = ABIChecker::check(
      "./" + ABIPkg + "/.mabi",
      "~/.marai/packages/" + ABIPkg + "/.mabi");
    result.print(OS);

    return result.isBlocking() ? 1 : 0;
  }

  WithColor::error() << "marai abi : sous-commande inconnue '" << ABISubCmd << "'.\n";
  OS << "Usage: marai abi check <package>\n";
  return 1;
}

static int cmdList(raw_ostream &OS) {
  InstallOptions Opts;
  Installer installer(Opts);
  installer.list(OS, ListDeps);
  return 0;
}

static int cmdRemove(raw_ostream &OS) {
  if (RemovePkg.empty()) {
    WithColor::error() << "marai remove : package manquant.\n";
    return 1;
  }
  InstallOptions Opts;
  Installer installer(Opts);
  return installer.remove(RemovePkg, OS) ? 0 : 1;
}

static int cmdUpdate(raw_ostream &OS) {
  InstallOptions Opts;
  Installer installer(Opts);
  auto results = installer.update("./Maralock.yaml");
  bool hasError = false;
  for (const auto &R : results) {
    R.print(OS, true);
    if (!R.success()) hasError = true;
  }
  return hasError ? 1 : 0;
}

static int cmdPublish(raw_ostream &OS) {
  if (PublishPath.empty()) {
    WithColor::error() << "marai publish : chemin manquant.\n";
    return 1;
  }
  InstallOptions Opts;
  Installer installer(Opts);
  return installer.publish(PublishPath, OS) ? 0 : 1;
}

//----------------------------------------------------------------------
// main
//----------------------------------------------------------------------

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  cl::SetVersionPrinter([](raw_ostream &OS) {
    OS << "marai v0.1 Naverta build 26160621 beta\n";
  });
  cl::ParseCommandLineOptions(argc, argv,
    "marai - Maratine Package Manager (MABI 1.0.1)\n");

  raw_ostream &OS = outs();

  if (VersionCmd) { printVersion(OS); return 0; }

  printHeader(OS);

  if (InstallCmd) return cmdInstall(OS);
  if (AuditCmd)   return cmdAudit(OS);
  if (ABICmd)     return cmdABI(OS);
  if (ListCmd)    return cmdList(OS);
  if (RemoveCmd)  return cmdRemove(OS);
  if (UpdateCmd)  return cmdUpdate(OS);
  if (PublishCmd) return cmdPublish(OS);

  OS << "Usage : marai <commande> [options]\n\n";
  OS << "Commandes :\n";
  OS << "  install  <pkg[@ver]>...    Installer des packages\n";
  OS << "  update                     Mettre à jour tous les packages\n";
  OS << "  remove   <pkg>             Supprimer un package\n";
  OS << "  list     [--deps]          Lister les packages installés\n";
  OS << "  audit    [--aude] [--json] Analyser les failles (--aude = mode ingénieur)\n";
  OS << "  publish  <path>            Publier un package\n";
  OS << "  abi      check <pkg>       Vérifier la compatibilité MABI 1.0.1\n";
  OS << "  version                    Afficher la version\n\n";
  OS << "Exemples :\n";
  OS << "  marai install MGCPhysics@2.1.0\n";
  OS << "  marai install --force MaraNet@1.9.0\n";
  OS << "  marai audit\n";
  OS << "  marai audit --aude --sbom\n";
  OS << "  marai audit --aude --json > rapport.json\n";
  OS << "  marai abi check MaraNet\n\n";
  return 0;
}
