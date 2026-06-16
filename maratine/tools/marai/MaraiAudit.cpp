//===-- MaraiAudit.cpp – Vulnerability Audit Engine ----------------------===//
//
// Maratine Package Manager — marai
// Auteur : Vyft Ltd — 2026
// Licence : Proprietary
//
//===----------------------------------------------------------------------===//

#include "MaraiAudit.h"
#include "MaraiABI.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/StringRef.h"
#include <algorithm>
#include <string>

using namespace llvm;
using namespace marai;

//----------------------------------------------------------------------
// Utilitaires d'affichage
//----------------------------------------------------------------------

static const char *ANSI_RESET   = "\033[0m";
static const char *ANSI_BOLD    = "\033[1m";
static const char *ANSI_RED     = "\033[31m";
static const char *ANSI_YELLOW  = "\033[33m";
static const char *ANSI_ORANGE  = "\033[38;5;208m";
static const char *ANSI_BLUE    = "\033[34m";
static const char *ANSI_GRAY    = "\033[90m";
static const char *ANSI_GREEN   = "\033[32m";
static const char *ANSI_CYAN    = "\033[36m";
static const char *ANSI_MAGENTA = "\033[35m";

StringRef marai::severityLabel(Severity S) {
  switch (S) {
  case Severity::CRITICAL: return "CRITICAL";
  case Severity::HIGH:     return "HIGH    ";
  case Severity::MEDIUM:   return "MEDIUM  ";
  case Severity::LOW:      return "LOW     ";
  default:                 return "INFO    ";
  }
}

StringRef marai::severityColor(Severity S) {
  switch (S) {
  case Severity::CRITICAL: return "\033[41;1m";  // fond rouge
  case Severity::HIGH:     return "\033[38;5;208m";
  case Severity::MEDIUM:   return "\033[33m";
  case Severity::LOW:      return "\033[34m";
  default:                 return "\033[90m";
  }
}

//----------------------------------------------------------------------
// AuditReport
//----------------------------------------------------------------------

void AuditReport::addFinding(const Vulnerability &V) {
  Findings.push_back(V);
  switch (V.Level) {
  case Severity::CRITICAL: ++CriticalCount; break;
  case Severity::HIGH:     ++HighCount;     break;
  case Severity::MEDIUM:   ++MediumCount;   break;
  case Severity::LOW:      ++LowCount;      break;
  default:                 ++InfoCount;     break;
  }
}

void AuditReport::print(raw_ostream &OS, bool Color) const {
  auto C = [&](const char *Code) -> StringRef {
    return Color ? Code : "";
  };

  OS << "\n";
  OS << C(ANSI_BOLD) << "marai audit — rapport de sécurité\n" << C(ANSI_RESET);
  OS << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";

  if (Findings.empty()) {
    OS << C(ANSI_GREEN) << "  ✓ Aucune faille détectée dans "
       << TotalPackages << " packages.\n" << C(ANSI_RESET);
    return;
  }

  // Trier par sévérité décroissante
  auto sorted = Findings;
  std::sort(sorted.begin(), sorted.end(), [](const Vulnerability &A,
                                              const Vulnerability &B) {
    return A.Level > B.Level;
  });

  for (const auto &V : sorted) {
    StringRef col = Color ? severityColor(V.Level) : StringRef("");
    OS << col << " " << severityLabel(V.Level) << " " << C(ANSI_RESET)
       << "  " << C(ANSI_BOLD) << V.CVEID << C(ANSI_RESET)
       << "  " << V.PackageName << "@" << V.AffectedVersion
       << "  CVSS " << V.CVSSScore << "\n";
    OS << "          " << V.Description << "\n";
    OS << "          " << C(ANSI_CYAN) << "Vecteur: " << V.Vector
       << C(ANSI_RESET) << "\n";
    if (!V.FixedVersion.empty())
      OS << "          " << C(ANSI_GREEN) << "Correction : upgrader vers "
         << V.PackageName << "@" << V.FixedVersion << C(ANSI_RESET) << "\n";
    OS << "          " << V.Remediation << "\n\n";
  }

  OS << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
  OS << C(ANSI_BOLD) << "Résumé : " << TotalPackages << " packages analysés\n"
     << C(ANSI_RESET);
  if (CriticalCount) OS << C(ANSI_RED)    << "  CRITICAL : " << CriticalCount << C(ANSI_RESET) << "\n";
  if (HighCount)     OS << C(ANSI_ORANGE) << "  HIGH     : " << HighCount     << C(ANSI_RESET) << "\n";
  if (MediumCount)   OS << C(ANSI_YELLOW) << "  MEDIUM   : " << MediumCount   << C(ANSI_RESET) << "\n";
  if (LowCount)      OS << C(ANSI_BLUE)   << "  LOW      : " << LowCount      << C(ANSI_RESET) << "\n";
  if (InfoCount)     OS << C(ANSI_GRAY)   << "  INFO     : " << InfoCount     << C(ANSI_RESET) << "\n";
  OS << "\n";
  if (hasBlockers())
    OS << C(ANSI_RED) << C(ANSI_BOLD)
       << "⚠ Des failles CRITICAL/HIGH requièrent une action immédiate.\n"
       << "  Utilisez : marai install --force <package>@<version-corrigée>\n"
       << C(ANSI_RESET);
}

void AuditReport::printJSON(raw_ostream &OS) const {
  OS << "{\n  \"mabi\": \"1.0.1\",\n";
  OS << "  \"totalPackages\": " << TotalPackages << ",\n";
  OS << "  \"counts\": {\n";
  OS << "    \"critical\": " << CriticalCount << ",\n";
  OS << "    \"high\": "     << HighCount     << ",\n";
  OS << "    \"medium\": "   << MediumCount   << ",\n";
  OS << "    \"low\": "      << LowCount      << ",\n";
  OS << "    \"info\": "     << InfoCount     << "\n  },\n";
  OS << "  \"findings\": [\n";
  for (size_t i = 0; i < Findings.size(); ++i) {
    const auto &V = Findings[i];
    OS << "    { \"cve\": \"" << V.CVEID
       << "\", \"package\": \"" << V.PackageName
       << "\", \"version\": \"" << V.AffectedVersion
       << "\", \"cvss\": " << V.CVSSScore
       << ", \"severity\": \"" << severityLabel(V.Level).trim() << "\""
       << ", \"fixedIn\": \"" << V.FixedVersion << "\" }";
    if (i + 1 < Findings.size()) OS << ",";
    OS << "\n";
  }
  OS << "  ]\n}\n";
}

//----------------------------------------------------------------------
// AudeReport::printAude — rapport avancé cybersec ingénieur
//----------------------------------------------------------------------

void AudeReport::printAude(raw_ostream &OS, bool Color) const {
  auto C = [&](const char *Code) -> StringRef {
    return Color ? Code : "";
  };

  // En-tête
  OS << "\n";
  OS << C(ANSI_MAGENTA) << C(ANSI_BOLD);
  OS << "╔══════════════════════════════════════════════════════════╗\n";
  OS << "║   marai audit --aude  —  Audit Cybersec Ingénieur        ║\n";
  OS << "║   MABI 1.0.1  ·  CVSS v3.1  ·  Slura/Maratine           ║\n";
  OS << "╚══════════════════════════════════════════════════════════╝\n";
  OS << C(ANSI_RESET) << "\n";

  // Section 1 : CVE standard
  OS << C(ANSI_BOLD) << "§1  Failles CVE par sévérité\n" << C(ANSI_RESET);
  OS << "────────────────────────────────────────────────────────────\n";
  print(OS, Color);

  // Section 2 : Matrice ABI MABI 1.0.1
  OS << C(ANSI_BOLD) << "\n§2  Compatibilité ABI — MABI 1.0.1\n" << C(ANSI_RESET);
  OS << "────────────────────────────────────────────────────────────\n";
  for (const auto &E : ABIMatrix) {
    StringRef col = (E.Status == "OK")       ? (Color ? ANSI_GREEN  : "") :
                    (E.Status == "BREAKING")  ? (Color ? ANSI_RED    : "") :
                                                (Color ? ANSI_YELLOW : "");
    OS << col << "  [" << E.Status << "] " << C(ANSI_RESET)
       << E.Package << "  — " << E.Detail << "\n";
  }

  // Section 3 : Audit cryptographique
  OS << C(ANSI_BOLD) << "\n§3  Audit cryptographique\n" << C(ANSI_RESET);
  OS << "────────────────────────────────────────────────────────────\n";
  if (CryptoIssues.empty()) {
    OS << C(ANSI_GREEN) << "  ✓ Aucun usage d'algorithme faible détecté.\n" << C(ANSI_RESET);
  } else {
    for (const auto &I : CryptoIssues) {
      StringRef col = Color ? severityColor(I.Level) : StringRef("");
      OS << col << " " << severityLabel(I.Level) << " " << C(ANSI_RESET)
         << "  " << I.Package << " : " << I.Algorithm
         << " — " << I.Reason << "\n";
    }
  }

  // Section 4 : Permissions
  OS << C(ANSI_BOLD) << "\n§4  Analyse des permissions (RAbstractallowing.xml)\n" << C(ANSI_RESET);
  OS << "────────────────────────────────────────────────────────────\n";
  if (PermIssues.empty()) {
    OS << C(ANSI_GREEN) << "  ✓ Aucune permission excessive détectée.\n" << C(ANSI_RESET);
  } else {
    for (const auto &P : PermIssues) {
      StringRef col = Color ? severityColor(P.Level) : StringRef("");
      OS << col << " " << severityLabel(P.Level) << " " << C(ANSI_RESET)
         << "  " << P.Package << " : " << P.Permission
         << " — " << P.Reason << "\n";
    }
  }

  // Section 5 : Exposition réseau
  OS << C(ANSI_BOLD) << "\n§5  Exposition réseau\n" << C(ANSI_RESET);
  OS << "────────────────────────────────────────────────────────────\n";
  if (NetExposures.empty()) {
    OS << C(ANSI_GREEN) << "  ✓ Aucune exposition réseau non chiffrée.\n" << C(ANSI_RESET);
  } else {
    for (const auto &N : NetExposures) {
      StringRef enc = N.Encrypted ? (Color ? ANSI_GREEN : "") : (Color ? ANSI_RED : "");
      OS << "  " << N.Package << " : " << N.Endpoint
         << " (" << N.Protocol << ") — "
         << enc << (N.Encrypted ? "chiffré" : "NON CHIFFRÉ") << C(ANSI_RESET) << "\n";
    }
  }

  // Section 6 : Intégrité supply chain
  OS << C(ANSI_BOLD) << "\n§6  Intégrité supply chain\n" << C(ANSI_RESET);
  OS << "────────────────────────────────────────────────────────────\n";
  if (ChainIssues.empty()) {
    OS << C(ANSI_GREEN) << "  ✓ Tous les packages vérifiés (SHA-256 + signature).\n" << C(ANSI_RESET);
  } else {
    for (const auto &Ch : ChainIssues) {
      StringRef col = Color ? severityColor(Ch.Level) : StringRef("");
      OS << col << " " << severityLabel(Ch.Level) << " " << C(ANSI_RESET)
         << "  " << Ch.Package << " : " << Ch.Issue << "\n";
    }
  }

  // Section 7 : SBOM résumé
  OS << C(ANSI_BOLD) << "\n§7  SBOM — Software Bill of Materials (" << SBOM.size() << " composants)\n" << C(ANSI_RESET);
  OS << "────────────────────────────────────────────────────────────\n";
  OS << C(ANSI_GRAY)
     << "  Utilisez 'marai audit --aude --sbom' pour l'export complet SPDX.\n"
     << C(ANSI_RESET);
  for (size_t i = 0; i < SBOM.size() && i < 10; ++i) {
    const auto &S = SBOM[i];
    OS << "  " << S.Name << "@" << S.Version
       << "  [" << S.License << "]  SHA256:" << S.SHA256.substr(0, 16) << "…\n";
  }
  if (SBOM.size() > 10)
    OS << "  … et " << (SBOM.size() - 10) << " autres.\n";

  // Pied de page
  OS << "\n" << C(ANSI_MAGENTA)
     << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
     << C(ANSI_RESET);
}

//----------------------------------------------------------------------
// AuditEngine::run — audit standard
//----------------------------------------------------------------------

AuditReport AuditEngine::run(StringRef LockfilePath, bool Color) {
  AuditReport Report;
  // TODO: parser Maralock.yaml et interroger le registry CVE
  // Les appels réels vers le registry Maratine sont effectués ici.
  Report.TotalPackages = 0;
  return Report;
}

//----------------------------------------------------------------------
// AuditEngine::runAude — audit cybersec avancé
//----------------------------------------------------------------------

AudeReport AuditEngine::runAude(StringRef LockfilePath,
                                 StringRef ProjectRoot, bool Color) {
  AudeReport Report;
  // Base CVE standard
  AuditReport base = run(LockfilePath, Color);
  static_cast<AuditReport &>(Report) = base;

  // Analyse ABI
  // TODO: parcourir les packages installés et vérifier MABI
  buildSBOM(LockfilePath, Report);
  checkSupplyChain(LockfilePath, Report);
  return Report;
}

void AuditEngine::buildSBOM(StringRef LockfilePath, AudeReport &R) {
  // TODO: parser Maralock.yaml → remplir R.SBOM avec Name/Version/License/SHA256
}

void AuditEngine::checkSupplyChain(StringRef LockfilePath, AudeReport &R) {
  // TODO: vérifier SHA-256 de chaque archive .marpkg contre le lockfile
}

void AuditEngine::auditCrypto(StringRef PackagePath, AudeReport &R) {
  // TODO: analyser les appels DrvAPIInterCon***Crypto*** dans les .mara
  // Détecter : MD5, SHA-1, RSA < 2048, AES-128, DES
}

void AuditEngine::auditPermissions(StringRef PackagePath, AudeReport &R) {
  // TODO: parser RAbstractallowing.xml et signaler les permissions excessives
  // NET_RAW, FS_ROOT, CAMERA sans justification → HIGH
}

void AuditEngine::auditNetwork(StringRef PackagePath, AudeReport &R) {
  // TODO: détecter les endpoints HTTP non chiffrés dans MaraNet calls
}

//----------------------------------------------------------------------
// AudeReport::printAudeSBOM — Software Bill of Materials
//----------------------------------------------------------------------

void AudeReport::printAudeSBOM(raw_ostream &OS) const {
  OS << "\n§ SBOM — Software Bill of Materials\n";
  OS << "────────────────────────────────────────────────────────\n";

  if (SBOM.empty()) {
    OS << "  (aucun package enregistré dans Maralock.yaml)\n";
    return;
  }

  OS << llvm::format("  %-30s %-12s %-20s %s\n",
                     "Package", "Version", "Licence", "SHA-256");
  OS << "  " << std::string(80, '-') << "\n";

  for (const auto &E : SBOM) {
    std::string sha = E.SHA256.size() > 16
                      ? E.SHA256.substr(0, 16) + "…"
                      : E.SHA256;
    OS << llvm::format("  %-30s %-12s %-20s %s\n",
                       E.Name.c_str(), E.Version.c_str(),
                       E.License.c_str(), sha.c_str());
  }

  OS << "\n  Total : " << SBOM.size() << " composant(s)\n";

  for (const auto &C : ChainIssues) {
    OS << "  ⚠ [" << C.Package << "] " << C.Issue << "\n";
  }
}
