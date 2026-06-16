//===-- MaraiABI.cpp – MABI 1.0.1 ABI Compatibility Checker -------------===//
//
// Maratine Package Manager — marai
// Auteur : Vyft Ltd — 2026
// Licence : Proprietary
//
//===----------------------------------------------------------------------===//

#include "MaraiABI.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/SmallString.h"
#include <map>
#include <sstream>
#include <cstdio>

using namespace llvm;
using namespace marai;

//----------------------------------------------------------------------
// TargetArch helpers
//----------------------------------------------------------------------

TargetArch marai::parseArch(StringRef S) {
  std::string lowStr = S.lower();
  StringRef low(lowStr);
  if (low == "arm64" || low == "aarch64") return TargetArch::ARM64;
  if (low == "x64"   || low == "amd64" || low == "x86_64") return TargetArch::X64;
  return TargetArch::Unknown;
}

StringRef marai::archName(TargetArch A) {
  switch (A) {
  case TargetArch::ARM64: return "arm64";
  case TargetArch::X64:   return "x64";
  default:                return "unknown";
  }
}

//----------------------------------------------------------------------
// CallingConvention
//----------------------------------------------------------------------

CallingConvention CallingConvention::forArch(TargetArch A) {
  switch (A) {
  case TargetArch::ARM64:
    return {TargetArch::ARM64, "AAPCS64", "x0-x7", "x0", "d0-d7", 8};
  case TargetArch::X64:
    return {TargetArch::X64, "Microsoft x64", "RCX,RDX,R8,R9", "RAX", "XMM0-XMM3", 4};
  default:
    return {TargetArch::Unknown, "Unknown", "", "", "", 0};
  }
}

void CallingConvention::print(raw_ostream &OS) const {
  OS << "  Convention : " << Name << "\n";
  OS << "  Args       : " << ArgRegs << " (max " << MaxRegArgs << " regs)\n";
  OS << "  Retour     : " << ReturnReg << "\n";
  OS << "  Flottants  : " << FloatArgRegs << "\n";
}

//----------------------------------------------------------------------
// MABIVersion::parse — accepte "MABI 1.0.1" et "1.0.1"
//----------------------------------------------------------------------

MABIVersion MABIVersion::parse(StringRef S) {
  StringRef digits = S.contains(' ') ? S.rsplit(' ').second : S;
  digits = digits.trim();

  unsigned maj = 1, min = 0, pat = 1;
  auto [majStr, rest1] = digits.split('.');
  auto [minStr, patStr] = rest1.split('.');
  majStr.trim().getAsInteger(10, maj);
  minStr.trim().getAsInteger(10, min);
  patStr.trim().getAsInteger(10, pat);
  return {maj, min, pat};
}

//----------------------------------------------------------------------
// ABIChecker::mangle
// ARM64 : _Mara_<Module>_<Symbol>_<Hash6>
// X64   : _Mara_x64_<Module>_<Symbol>_<Hash6>
//----------------------------------------------------------------------

std::string ABIChecker::mangle(StringRef Module, StringRef Symbol,
                                TargetArch Arch) {
  uint32_t h = 5381;
  for (char c : Module) h = ((h << 5) + h) ^ (unsigned char)c;
  for (char c : Symbol) h = ((h << 5) + h) ^ (unsigned char)c;
  h &= 0xFFFFFF;

  char hex[8];
  snprintf(hex, sizeof(hex), "%06X", h);

  std::string prefix = (Arch == TargetArch::X64) ? "_Mara_x64_" : "_Mara_";
  return prefix + Module.str() + "_" + Symbol.str() + "_" + hex;
}

//----------------------------------------------------------------------
// ABIChecker::validateMangling
//----------------------------------------------------------------------

bool ABIChecker::validateMangling(StringRef Name, TargetArch Arch) {
  if (Arch == TargetArch::X64) {
    if (!Name.starts_with("_Mara_x64_")) return false;
    StringRef rest = Name.drop_front(10);
    return rest.count('_') >= 2;
  }
  if (!Name.starts_with("_Mara_")) return false;
  if (Name.starts_with("_Mara_x64_")) return false; // X64 dans un contexte ARM64
  StringRef rest = Name.drop_front(6);
  return rest.count('_') >= 2;
}

//----------------------------------------------------------------------
// ABIChecker::demangle
//----------------------------------------------------------------------

bool ABIChecker::demangle(StringRef Name, std::string &OutModule,
                           std::string &OutSymbol, TargetArch &OutArch) {
  StringRef rest;
  if (Name.starts_with("_Mara_x64_")) {
    OutArch = TargetArch::X64;
    rest = Name.drop_front(10);
  } else if (Name.starts_with("_Mara_")) {
    OutArch = TargetArch::ARM64;
    rest = Name.drop_front(6);
  } else {
    return false;
  }

  // rest = Module_Symbol_Hash6
  auto [modPart, symHash] = rest.split('_');
  auto [symPart, hashPart] = symHash.rsplit('_');

  if (modPart.empty() || symPart.empty() || hashPart.size() != 6)
    return false;

  OutModule = modPart.str();
  OutSymbol = symPart.str();
  return true;
}

//----------------------------------------------------------------------
// Parseur YAML ligne-à-ligne pour le format .mabi
//----------------------------------------------------------------------

static std::string yamlValue(StringRef line) {
  auto colon = line.find(':');
  if (colon == StringRef::npos) return "";
  StringRef val = line.substr(colon + 1).trim();
  // Enlève les guillemets
  if (val.starts_with("\"") && val.ends_with("\""))
    val = val.slice(1, val.size() - 1);
  return val.str();
}

static bool yamlBool(StringRef line) {
  std::string v = yamlValue(line);
  StringRef sv(v);
  return sv.lower() == "true" || sv == "1";
}

//----------------------------------------------------------------------
// ABIChecker::loadManifest — parse un fichier .mabi YAML
//----------------------------------------------------------------------

std::vector<ABIEntry> ABIChecker::loadManifest(StringRef ManifestPath) {
  std::vector<ABIEntry> entries;

  auto BufOrErr = MemoryBuffer::getFile(ManifestPath);
  if (!BufOrErr) return entries;

  StringRef content = (*BufOrErr)->getBuffer();
  SmallVector<StringRef, 64> lines;
  content.split(lines, '\n');

  TargetArch fileArch = TargetArch::ARM64;
  bool inEntries = false;
  ABIEntry current;
  bool building = false;

  for (StringRef rawLine : lines) {
    StringRef line = rawLine.trim();
    if (line.empty() || line.starts_with("#")) continue;

    if (line.starts_with("arch:")) {
      fileArch = parseArch(yamlValue(line));
      continue;
    }
    if (line.starts_with("entries:")) {
      inEntries = true;
      continue;
    }
    if (!inEntries) continue;

    if (line.starts_with("- symbol:")) {
      if (building && !current.Symbol.empty())
        entries.push_back(current);
      current = ABIEntry{};
      current.Arch = fileArch;
      current.Symbol = yamlValue(line);
      building = true;
    } else if (line.starts_with("module:") && building) {
      current.Module = yamlValue(line);
    } else if (line.starts_with("signature:") && building) {
      current.Signature = yamlValue(line);
    } else if (line.starts_with("public:") && building) {
      current.IsPublic = yamlBool(line);
    }
  }
  if (building && !current.Symbol.empty())
    entries.push_back(current);

  return entries;
}

//----------------------------------------------------------------------
// ABIChecker::check — compare deux manifestes .mabi
//----------------------------------------------------------------------

ABICheckResult ABIChecker::check(StringRef InstalledPath,
                                  StringRef RequiredPath) {
  ABICheckResult result;
  result.InstalledVersion = MABIVersion::current();
  result.RequiredVersion  = MABIVersion::current();

  auto installed = loadManifest(InstalledPath);
  auto required  = loadManifest(RequiredPath);

  // Si les deux manifestes sont absents → Unknown
  if (installed.empty() && required.empty()) {
    result.Level = ABICompatLevel::Compatible;
    return result;
  }
  if (installed.empty() || required.empty()) {
    result.Level = ABICompatLevel::BreakingChange;
    if (installed.empty())
      result.Breaking.push_back("Manifeste installé introuvable : " +
                                  InstalledPath.str());
    else
      result.Breaking.push_back("Manifeste requis introuvable : " +
                                  RequiredPath.str());
    return result;
  }

  // Détection d'architecture
  result.InstalledArch = installed.empty() ? TargetArch::Unknown
                                           : installed.front().Arch;
  result.RequiredArch  = required.empty()  ? TargetArch::Unknown
                                           : required.front().Arch;

  if (result.InstalledArch != TargetArch::Unknown &&
      result.RequiredArch  != TargetArch::Unknown &&
      result.InstalledArch != result.RequiredArch) {
    result.Level = ABICompatLevel::ArchMismatch;
    result.Breaking.push_back(
      "Architecture incompatible : installé=" +
      archName(result.InstalledArch).str() + ", requis=" +
      archName(result.RequiredArch).str());
    return result;
  }

  // Indexe les symboles installés
  std::map<std::string, ABIEntry> instMap;
  for (const auto &E : installed)
    instMap[E.Symbol] = E;

  bool hasBreaking = false;

  for (const auto &req : required) {
    if (!req.IsPublic) continue;

    auto it = instMap.find(req.Symbol);
    if (it == instMap.end()) {
      result.Breaking.push_back(req.Symbol + " : symbole absent");
      hasBreaking = true;
    } else if (it->second.Signature != req.Signature) {
      result.Breaking.push_back(req.Symbol + " : signature modifiée  " +
                                  it->second.Signature + "  →  " +
                                  req.Signature);
      hasBreaking = true;
      instMap.erase(it);
    } else {
      instMap.erase(it);
    }
  }

  for (const auto &[sym, entry] : instMap)
    if (entry.IsPublic) result.Added.push_back(sym);

  if (hasBreaking)
    result.Level = ABICompatLevel::BreakingChange;
  else if (!result.Added.empty())
    result.Level = ABICompatLevel::BackwardCompat;
  else
    result.Level = ABICompatLevel::Compatible;

  return result;
}

//----------------------------------------------------------------------
// ABIChecker::generateManifest — génère un .mabi vide
//----------------------------------------------------------------------

void ABIChecker::generateManifest(StringRef PackageName, TargetArch Arch,
                                   raw_ostream &OS) {
  OS << "mabi: \"1.0.1\"\n";
  OS << "arch: \"" << archName(Arch) << "\"\n";
  OS << "package: \"" << PackageName << "\"\n";
  OS << "entries:\n";
  OS << "  # - symbol: \"" << mangle(PackageName, "ExampleFn", Arch) << "\"\n";
  OS << "  #   module: \"" << PackageName << "\"\n";
  OS << "  #   signature: \"(i32)->i32\"\n";
  OS << "  #   public: true\n";
}

//----------------------------------------------------------------------
// ABICheckResult::print
//----------------------------------------------------------------------

void ABICheckResult::print(raw_ostream &OS) const {
  OS << "  ABI : " << InstalledVersion.str();
  if (InstalledArch != TargetArch::Unknown)
    OS << " [" << archName(InstalledArch) << "]";
  OS << "  ←  requis " << RequiredVersion.str();
  if (RequiredArch != TargetArch::Unknown)
    OS << " [" << archName(RequiredArch) << "]";
  OS << "\n";

  switch (Level) {
  case ABICompatLevel::Compatible:
    OS << "  \033[32m✓ Compatible\033[0m\n";
    break;
  case ABICompatLevel::BackwardCompat:
    OS << "  \033[33m△ Backward-compatible — "
       << Added.size() << " nouveau(x) symbole(s)\033[0m\n";
    for (const auto &A : Added)
      OS << "      + " << A << "\n";
    break;
  case ABICompatLevel::BreakingChange:
    OS << "  \033[31m✗ BREAKING CHANGE — "
       << Breaking.size() << " symbole(s) cassé(s)\033[0m\n";
    for (const auto &B : Breaking)
      OS << "      ✗ " << B << "\n";
    break;
  case ABICompatLevel::ArchMismatch:
    OS << "  \033[31m✗ ARCHITECTURE INCOMPATIBLE\033[0m\n";
    for (const auto &B : Breaking)
      OS << "      ✗ " << B << "\n";
    break;
  case ABICompatLevel::VersionMismatch:
    OS << "  \033[31m✗ VERSION MABI INCOMPATIBLE\033[0m\n";
    break;
  }
  for (const auto &W : Warnings)
    OS << "    ! " << W << "\n";
}
