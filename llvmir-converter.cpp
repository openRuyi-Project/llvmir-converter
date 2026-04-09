//===--- llvmir-converter.cpp - Convert LLVM IR to ELF -------------------===//
//
// Copyright 2026 Institute of Software, Chinese Academy of Sciences (ISCAS)
// Author: YunQiang Su <yunqiang@isrc.iscas.ac.cn>
// SPDX-License-Identifier: Apache-2.0
//
//===----------------------------------------------------------------------===//
//
// This tool converts LLVM IR bitcode files to ELF executables/shared libraries.
// It reads a _cmd script file to determine compilation parameters and uses
// a template LL file to set target features.
//
//===----------------------------------------------------------------------===//

#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/CodeGen/CommandFlags.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ModuleSummaryIndex.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/TargetParser/RISCVISAInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Config/llvm-config.h"
#include <memory>
#include <optional>
#include <sstream>
#include <map>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <algorithm>
#include <cctype>
#include <regex>
#include <vector>

// Constants for parsing
// Length of "-Wl,--version-script=" prefix (sizeof includes null terminator, so subtract 1)
static constexpr size_t VERSION_SCRIPT_ARG_LEN = sizeof("-Wl,--version-script=") - 1;

using namespace llvm;

static codegen::RegisterCodeGenFlags CGF;

cl::OptionCategory ConverterCat("llvmir-converter Options");

static cl::opt<std::string> InputCmdFile(cl::Positional,
                                         cl::desc("<input _cmd file>"),
                                         cl::init("-"));

static cl::opt<std::string> TemplatePath("t",
                                         cl::desc("Template LL file path"),
                                         cl::value_desc("filename"),
                                         cl::cat(ConverterCat));

static cl::opt<std::string> OutputDir("o",
                                      cl::desc("Output directory"),
                                      cl::value_desc("directory"),
                                      cl::init("output"),
                                      cl::cat(ConverterCat));

static cl::opt<std::string> PgoOutputDir("pgo-output",
                                         cl::desc("PGO instrumented output directory"),
                                         cl::value_desc("directory"),
                                         cl::cat(ConverterCat));

static cl::opt<std::string> PgoProfilesOutputDir("pgo-profiles-output",
                                                 cl::desc("Directory for PGO .profraw output at runtime"),
                                                 cl::value_desc("directory"),
                                                 cl::cat(ConverterCat));

static cl::opt<bool> KeepTemp("keep-temp",
                              cl::desc("Keep temporary files"),
                              cl::cat(ConverterCat));

static cl::opt<bool> Verbose("v",
                             cl::desc("Enable verbose output"),
                             cl::cat(ConverterCat));

static cl::opt<bool> DryRun("dry-run",
                            cl::desc("Parse and validate without executing"),
                            cl::cat(ConverterCat));

static cl::opt<bool> Incremental("incremental",
                                 cl::desc("Skip files if output is newer than input"),
                                 cl::cat(ConverterCat));

static cl::opt<bool> ShowVersion("tool-version",
                                 cl::desc("Show tool version information"),
                                 cl::cat(ConverterCat));

// Version information
static const char* TOOL_VERSION = "1.0.0";
static const char* TOOL_NAME = "llvmir-converter";

//===----------------------------------------------------------------------===//
// Verbose Output Utilities
//===----------------------------------------------------------------------===//

/// Macro for verbose output - only prints when -v flag is set
#define VERBOSE_OUT(msg) \
  do { \
    if (Verbose) { \
      outs() << "[VERBOSE] " << msg; \
    } \
  } while(0)

/// Overloaded verbose output for different types
template<typename T>
static void verbosePrint(const T &msg) {
  if (Verbose) {
    outs() << "[VERBOSE] " << msg;
  }
}

/// Print file timestamp info in verbose mode
static void verbosePrintTimestamp(const std::string &Path, const std::string &Label) {
  if (!Verbose) return;
  
  struct stat St;
  if (stat(Path.c_str(), &St) == 0) {
    char TimeBuf[64];
    strftime(TimeBuf, sizeof(TimeBuf), "%Y-%m-%d %H:%M:%S",
             localtime(&St.st_mtime));
    outs() << "[VERBOSE] " << Label << " timestamp: " << TimeBuf << "\n";
  }
}

//===----------------------------------------------------------------------===//
// Incremental Compilation Support
//===----------------------------------------------------------------------===//

static bool shouldSkipIncremental(const std::vector<std::string> &InputPaths,
                                  const std::string &OutputPath) {
  struct stat OutputStat;

  if (stat(OutputPath.c_str(), &OutputStat) != 0) {
    VERBOSE_OUT("Output file does not exist, compilation required\n");
    return false;
  }

  for (const auto &InputPath : InputPaths) {
    struct stat InputStat;
    if (stat(InputPath.c_str(), &InputStat) != 0) {
      VERBOSE_OUT("Input file does not exist: " << InputPath << "\n");
      return false;
    }
    if (InputStat.st_mtime >= OutputStat.st_mtime) {
      VERBOSE_OUT("Input is newer than output, compilation required: " << InputPath << "\n");
      return false;
    }
  }

  outs() << "Skipping inputs (output is newer): " << OutputPath << "\n";
  return true;
}

//===----------------------------------------------------------------------===//
// Version Display
//===----------------------------------------------------------------------===//

/// Display version information
static void showVersionInfo() {
  outs() << TOOL_NAME << " version " << TOOL_VERSION << "\n";
  outs() << "Built with LLVM " << LLVM_VERSION_MAJOR << "." 
         << LLVM_VERSION_MINOR << "." << LLVM_VERSION_PATCH << "\n";
  outs() << "\nCopyright 2026 Institute of Software, Chinese Academy of Sciences (ISCAS)\n";
  outs() << "Author: YunQiang Su <yunqiang@isrc.iscas.ac.cn>\n";
  outs() << "License: Apache-2.0\n";
}

static void showSupportedTargets() {
  outs() << "Supported target architectures:\n";
  outs() << "  - x86\n";
  outs() << "  - x86_64\n";
  outs() << "  - arm\n";
  outs() << "  - aarch64\n";
  outs() << "  - riscv32\n";
  outs() << "  - riscv64\n";
  outs() << "  - mips\n";
  outs() << "  - mips64\n";
  outs() << "  - powerpc\n";
  outs() << "  - powerpc64\n";
  outs() << "  - sparc\n";
  outs() << "  - sparcv9\n";
  outs() << "\nActual support depends on the linked LLVM build.\n";
}

//===----------------------------------------------------------------------===//
// _cmd Script Parsing
//===----------------------------------------------------------------------===//

struct CmdInfo {
  std::string InputFile;       // Input bitcode file
  std::vector<std::string> InputFiles; // All input bitcode files
  std::string OutputFile;      // Output file path in script
  std::string VersionScript;   // Version script file for linker
  std::vector<std::string> CommandArgs; // Parsed clang command arguments
};

static std::string trim(const std::string &Str) {
  size_t Start = Str.find_first_not_of(" \t\n\r");
  if (Start == std::string::npos)
    return "";
  size_t End = Str.find_last_not_of(" \t\n\r");
  return Str.substr(Start, End - Start + 1);
}

static bool startsWith(StringRef Str, StringRef Prefix) {
  return Str.size() >= Prefix.size() && Str.substr(0, Prefix.size()) == Prefix;
}

static bool isLikelyInputPath(const std::string &Token) {
  if (!(startsWith(Token, "./") || startsWith(Token, "../")))
    return false;
  if (Token.find("_verscript") != std::string::npos)
    return false;
  if (Token.find("/output/") != std::string::npos || startsWith(Token, "output/"))
    return false;
  return true;
}

static bool optionTakesValue(const std::string &Token) {
  static const std::vector<std::string> Options = {
    "-L", "-I", "-F", "-B", "-l", "-x", "-target", "--target",
    "-isystem", "-iquote", "-idirafter", "-include", "-isysroot",
    "--sysroot", "-resource-dir", "-Xlinker", "-Xclang", "-mllvm"
  };
  return std::find(Options.begin(), Options.end(), Token) != Options.end();
}

static std::string stripOuterQuotes(const std::string &Text) {
  std::string Str = trim(Text);
  if (Str.size() >= 2 && (Str.front() == '"' || Str.front() == '\'')) {
    char Quote = Str.front();
    bool Escaped = false;
    for (size_t I = 1; I < Str.size(); ++I) {
      char C = Str[I];
      if (Escaped) {
        Escaped = false;
        continue;
      }
      if (C == '\\' && Quote == '"') {
        Escaped = true;
        continue;
      }
      if (C == Quote)
        return Str.substr(1, I - 1);
    }
  }
  return Str;
}

static bool hasClosingQuote(const std::string &Text, char Quote) {
  bool Escaped = false;
  for (size_t I = 1; I < Text.size(); ++I) {
    char C = Text[I];
    if (Escaped) {
      Escaped = false;
      continue;
    }
    if (C == '\\' && Quote == '"') {
      Escaped = true;
      continue;
    }
    if (C == Quote)
      return true;
  }
  return false;
}

static std::vector<std::string> shellTokenize(const std::string &Text) {
  std::vector<std::string> Tokens;
  std::string Current;
  bool InSingleQuote = false;
  bool InDoubleQuote = false;
  bool Escaped = false;

  for (size_t I = 0; I < Text.size(); ++I) {
    char C = Text[I];

    if (Escaped) {
      Current.push_back(C);
      Escaped = false;
      continue;
    }

    if (C == '\\' && !InSingleQuote) {
      if (I + 1 < Text.size() && Text[I + 1] == '\n') {
        ++I;
        continue;
      }
      Escaped = true;
      continue;
    }

    if (C == '\'' && !InDoubleQuote) {
      InSingleQuote = !InSingleQuote;
      continue;
    }

    if (C == '"' && !InSingleQuote) {
      InDoubleQuote = !InDoubleQuote;
      continue;
    }

    if (!InSingleQuote && !InDoubleQuote && C == '#' && Current.empty()) {
      while (I < Text.size() && Text[I] != '\n')
        ++I;
      continue;
    }

    if (!InSingleQuote && !InDoubleQuote && std::isspace(static_cast<unsigned char>(C))) {
      if (!Current.empty()) {
        Tokens.push_back(Current);
        Current.clear();
      }
      continue;
    }

    Current.push_back(C);
  }

  if (!Current.empty())
    Tokens.push_back(Current);
  return Tokens;
}

static void populateCmdInfo(CmdInfo &Info, const std::vector<std::string> &Args) {
  Info.CommandArgs = Args;
  std::vector<std::string> InputCandidates;
  enum class PendingValue { None, Output, VersionScript, Skip };
  PendingValue Pending = PendingValue::None;

  for (const auto &Token : Args) {
    if (Pending == PendingValue::Output) {
      Info.OutputFile = Token;
      Pending = PendingValue::None;
      continue;
    }
    if (Pending == PendingValue::VersionScript) {
      Info.VersionScript = Token;
      Pending = PendingValue::None;
      continue;
    }
    if (Pending == PendingValue::Skip) {
      Pending = PendingValue::None;
      continue;
    }

    if (Token == "-o" || Token == "--output") {
      Pending = PendingValue::Output;
      continue;
    }
    if (startsWith(Token, "--output=")) {
      Info.OutputFile = Token.substr(sizeof("--output=") - 1);
      continue;
    }
    if (startsWith(Token, "-o") && Token.size() > 2) {
      Info.OutputFile = Token.substr(2);
      continue;
    }

    if (Token == "--version-script") {
      Pending = PendingValue::VersionScript;
      continue;
    }
    if (startsWith(Token, "--version-script=")) {
      Info.VersionScript = Token.substr(sizeof("--version-script=") - 1);
      continue;
    }
    if (startsWith(Token, "-Wl,--version-script=")) {
      Info.VersionScript = Token.substr(VERSION_SCRIPT_ARG_LEN);
      continue;
    }
    if (startsWith(Token, "-Wl,--version-script,")) {
      Info.VersionScript = Token.substr(sizeof("-Wl,--version-script,") - 1);
      continue;
    }

    if (optionTakesValue(Token)) {
      Pending = PendingValue::Skip;
      continue;
    }

    if (isLikelyInputPath(Token))
      InputCandidates.push_back(Token);
  }

  if (!InputCandidates.empty())
    Info.InputFile = InputCandidates.front();
  Info.InputFiles = InputCandidates;
}

// Simple parsing to extract input/output files and version script
static CmdInfo parseCmdScript(const std::string &CmdPath) {
  CmdInfo Info;
  std::ifstream File(CmdPath);
  if (!File.is_open()) {
    errs() << "Error: Could not open _cmd file: " << CmdPath << "\n";
    return Info;
  }

  bool InCmdArray = false;
  std::string ArrayText;
  std::string Line;
  while (std::getline(File, Line)) {
    std::string TrimmedLine = trim(Line);

    if (InCmdArray) {
      if (TrimmedLine == ")") {
        populateCmdInfo(Info, shellTokenize(ArrayText));
        return Info;
      }
      ArrayText += Line;
      ArrayText += '\n';
      continue;
    }

    Line = TrimmedLine;

    // Skip empty lines and comments
    if (Line.empty() || Line[0] == '#')
      continue;

    size_t CmdPos = Line.find("cmd=");
    if (CmdPos == std::string::npos)
      continue;

    std::string Rhs = trim(Line.substr(CmdPos + 4));
    if (startsWith(Rhs, "(")) {
      std::string Rest = trim(Rhs.substr(1));
      if (Rest == ")") {
        populateCmdInfo(Info, {});
        return Info;
      }
      size_t CloseParen = Rest.rfind(')');
      if (CloseParen != std::string::npos && trim(Rest.substr(CloseParen + 1)).empty()) {
        ArrayText = Rest.substr(0, CloseParen);
        populateCmdInfo(Info, shellTokenize(ArrayText));
        return Info;
      }
      ArrayText = Rest + "\n";
      InCmdArray = true;
      continue;
    }

    if (Rhs.size() >= 1 && (Rhs.front() == '"' || Rhs.front() == '\'')) {
      char Quote = Rhs.front();
      std::string QuotedCommand = Rhs;
      while (!hasClosingQuote(QuotedCommand, Quote) && std::getline(File, Line)) {
        QuotedCommand += '\n';
        QuotedCommand += Line;
      }
      populateCmdInfo(Info, shellTokenize(stripOuterQuotes(QuotedCommand)));
      return Info;
    }

    populateCmdInfo(Info, shellTokenize(stripOuterQuotes(Rhs)));
    return Info;
  }

  return Info;
}

//===----------------------------------------------------------------------===//
// Feature Processing (from llvm-altattr)
//===----------------------------------------------------------------------===//
//
// This section handles the parsing and merging of target feature strings.
// Feature strings are comma-separated lists of features with prefixes:
//   - "+feature" means the feature is enabled
//   - "-feature" means the feature is disabled
//   - "feature" (no prefix) is treated as enabled
//
// Example feature strings:
//   - "+64bit,+sse,+sse2"         -> enable 64-bit, SSE, SSE2
//   - "+avx,-sse4.1"              -> enable AVX, disable SSE4.1
//   - "+m,+a,+f,+d,+c"            -> RISC-V MAFDC extensions
//
// When merging features, the override (second) features take precedence.
// This allows templates to override or extend existing feature sets.
//
//===----------------------------------------------------------------------===//

/// Parse a feature string into a map of feature -> enabled/disabled.
///
/// \param FeaturesStr A comma-separated feature string (e.g., "+zbs,+c,-a")
/// \return A map where keys are feature names and values indicate enabled (true) or disabled (false)
///
/// Example:
///   Input:  "+zbs,+c,-a"
///   Output: {"zbs": true, "c": true, "a": false}
static std::map<std::string, bool> parseFeatures(StringRef FeaturesStr) {
  std::map<std::string, bool> Features;
  if (FeaturesStr.empty())
    return Features;

  std::istringstream ss(FeaturesStr.str());
  std::string token;
  
  // Split by comma and process each feature
  while (std::getline(ss, token, ',')) {
    if (token.empty())
      continue;
    
    // Trim whitespace from token
    size_t start = token.find_first_not_of(" \t");
    size_t end = token.find_last_not_of(" \t");
    if (start == std::string::npos)
      continue;
    token = token.substr(start, end - start + 1);

    // Parse prefix: '+' enables, '-' disables, no prefix enables by default
    if (token[0] == '+') {
      Features[token.substr(1)] = true;   // Enable feature
    } else if (token[0] == '-') {
      Features[token.substr(1)] = false;  // Disable feature
    } else {
      Features[token] = true;             // No prefix -> enable by default
    }
  }
  return Features;
}

/// Merge two feature strings with override semantics.
///
/// \param BaseFeatures The base feature string (existing features)
/// \param OverrideFeatures The override feature string (new features to apply)
/// \return A merged feature string where OverrideFeatures takes precedence
///
/// This function is used when:
/// 1. Merging template features with command-line features
/// 2. Merging existing function features with new target features
///
/// Example:
///   Base:     "+sse,+sse2,-avx"
///   Override: "+avx,+avx2"
///   Result:   "+avx,+avx2,+sse,+sse2"  (avx is now enabled, sse/sse2 preserved)
static std::string mergeFeatures(StringRef BaseFeatures, StringRef OverrideFeatures) {
  // Parse both feature strings into maps
  auto Features = parseFeatures(BaseFeatures);
  auto Override = parseFeatures(OverrideFeatures);

  // Merge: override features take precedence over base features
  // This means if a feature appears in both, the OverrideFeatures setting wins
  for (const auto &KV : Override) {
    Features[KV.first] = KV.second;
  }

  // Reconstruct the feature string from the merged map
  // Format: "+feature" for enabled, "-feature" for disabled
  std::string Result;
  for (const auto &KV : Features) {
    if (!Result.empty())
      Result += ",";
    Result += (KV.second ? "+" : "-") + KV.first;
  }
  return Result;
}

static bool setProfileFilename(Module &M, const std::string &ProfileFilename) {
  if (ProfileFilename.empty())
    return true;

  LLVMContext &Context = M.getContext();
  Constant *Initializer = ConstantDataArray::getString(Context, ProfileFilename, true);
  auto *ArrayTy = cast<ArrayType>(Initializer->getType());

  if (GlobalVariable *Existing = M.getGlobalVariable("__llvm_profile_filename", true)) {
    if (!Existing->use_empty() && Existing->getValueType() != ArrayTy) {
      errs() << "Error: Existing __llvm_profile_filename has incompatible uses\n";
      return false;
    }

    if (Existing->getValueType() == ArrayTy) {
      Existing->setInitializer(Initializer);
      Existing->setConstant(true);
      Existing->setVisibility(GlobalValue::HiddenVisibility);
      Existing->setComdat(M.getOrInsertComdat("__llvm_profile_filename"));
      return true;
    }

    Existing->eraseFromParent();
  }

  auto *GV = new GlobalVariable(M, ArrayTy, true, GlobalValue::ExternalLinkage,
                                Initializer, "__llvm_profile_filename");
  GV->setVisibility(GlobalValue::HiddenVisibility);
  GV->setComdat(M.getOrInsertComdat("__llvm_profile_filename"));
  return true;
}

//===----------------------------------------------------------------------===//
// IR Processing
//===----------------------------------------------------------------------===//
//
// This section handles the core IR transformation process:
// 1. Load the input LLVM IR module (bitcode or textual IR)
// 2. Optionally load a template module to extract target features
// 3. Merge target features into all functions of the input module
// 4. Handle special cases like RISC-V ISA metadata
// 5. Write the modified module as bitcode
//
// The target-features attribute controls which CPU features the generated
// code can use. This is crucial for:
// - Ensuring code runs on the target CPU
// - Enabling/disabling specific instruction set extensions
// - Cross-compilation scenarios
//
//===----------------------------------------------------------------------===//

/// Process an LLVM IR file, applying target feature transformations.
///
/// \param InputPath Path to the input IR file (bitcode or .ll)
/// \param OutputPath Path where the processed bitcode will be written
/// \param TemplatePath Optional path to a template LL file with target features
/// \return true on success, false on failure
///
/// Processing steps:
/// 1. Load input IR module into memory
/// 2. If template is specified, extract its target-features
/// 3. Merge features: template features -> command-line features -> function features
/// 4. For RISC-V, update the module's riscv-isa metadata
/// 5. Write the transformed module as bitcode
static bool processIR(const std::string &InputPath,
                      const std::string &OutputPath,
                      const std::string &TemplatePath,
                      const std::optional<std::string> &ProfileFilename = std::nullopt) {
  LLVMContext Context;
  SMDiagnostic Err;
  
  // Step 1: Load the input IR module
  // parseIRFile can handle both bitcode (.bc) and textual IR (.ll) files
  std::unique_ptr<Module> M = parseIRFile(InputPath, Err, Context);
  if (!M) {
    Err.print("llvmir-converter", errs());
    return false;
  }

  // Get default features from codegen flags (can be set via command line)
  std::string FeaturesStr = codegen::getFeaturesStr();
  std::string TemplateFeaturesStr;

  // Step 2: Load template module if specified
  // Template provides a way to specify target features without modifying the IR
  if (!TemplatePath.empty()) {
    std::unique_ptr<Module> TM = parseIRFile(TemplatePath, Err, Context);
    if (!TM) {
      Err.print("llvmir-converter", errs());
      errs() << "Error: Could not load template file: " << TemplatePath << "\n";
      return false;
    } else {
      // Extract target-features from any function in the template
      // Usually templates have one placeholder function with the desired features
      for (Function &F : *TM) {
        TemplateFeaturesStr = F.getFnAttribute("target-features")
                               .getValueAsString().str();
        if (!TemplateFeaturesStr.empty()) {
          // Merge template features with command-line features
          // Template features serve as the base, command-line features override
          FeaturesStr = mergeFeatures(TemplateFeaturesStr, FeaturesStr);
          break;  // Use features from the first function found
        }
      }
    }
  }

  // Step 3: Apply merged features to all functions in the input module
  // Each function may have its own target-features; we merge with the new ones
  for (Function &F : *M) {
    std::string ExistingFeatures = F.getFnAttribute("target-features")
                                   .getValueAsString().str();
    // New features take precedence over existing function features
    std::string MergedFeatures = mergeFeatures(ExistingFeatures, FeaturesStr);
    F.addFnAttr("target-features", MergedFeatures);
  }

  // Step 4: Handle RISC-V specific metadata
  // RISC-V uses module-level metadata to track the ISA string
  MDNode *MD = static_cast<MDNode *>(M->getModuleFlag("riscv-isa"));
  if (MD) {
    // Determine if this is rv64 (64-bit) or rv32 (32-bit)
    bool rv64 = static_cast<MDString *>(MD->getOperand(0).get())->getString().starts_with("rv64");
    unsigned bits = rv64 ? 64 : 32;
    
    // Find a function with target-features to get the complete feature list
    for (Function &F : *M) {
      FeaturesStr = F.getFnAttribute("target-features")
                     .getValueAsString().str();
      if (!FeaturesStr.empty()) {
        // Parse the feature string into individual features
        std::vector<std::string> Features;
        std::istringstream ss(FeaturesStr);
        std::string s;
        while(std::getline(ss, s, ','))
          Features.push_back(s);
        
        // Use LLVM's RISC-V ISA parser to generate a canonical ISA string
        auto ParseResult = llvm::RISCVISAInfo::parseFeatures(bits, Features);
        if (!ParseResult) {
          errs() << "Warning: Failed to parse RISC-V features\n";
          break;
        }
        // Update the module's riscv-isa metadata with the new ISA string
        MD->replaceOperandWith(0, MDString::get(Context, (*ParseResult)->toString()));
        break;
      }
    }
  }

  if (ProfileFilename && !setProfileFilename(*M, *ProfileFilename))
    return false;

  // Step 5: Write the processed module as bitcode
  std::error_code EC;
  raw_fd_ostream Out(OutputPath, EC, sys::fs::OF_None);
  if (EC) {
    errs() << "Error writing output: " << EC.message() << "\n";
    return false;
  }
  WriteBitcodeToFile(*M, Out);
  Out.close();
  
  return true;
}

//===----------------------------------------------------------------------===//
// Clang Version Utilities
//===----------------------------------------------------------------------===//

/// Get the clang version suffix (e.g., "22" for clang-22)
static std::string getClangVersionSuffix() {
  return std::to_string(LLVM_VERSION_MAJOR);
}

static std::string replaceClangArg(const std::string &Arg, const std::string &VersionSuffix) {
  std::regex ClangPattern(R"(^((?:(?:/usr/bin/|/usr/local/bin/|/opt/llvm/bin/|/opt/.*/bin/)?clang)(?:\+\+|cpp)?)(-\d+)?$)");
  return std::regex_replace(Arg, ClangPattern, "$1-" + VersionSuffix);
}

//===----------------------------------------------------------------------===//
// Path Utilities
//===----------------------------------------------------------------------===//

static std::string getParentDirName(const std::string &Path) {
  std::filesystem::path p(Path);
  if (p.has_parent_path()) {
    return p.parent_path().filename().string();
  }
  return "";
}

static std::string getFileName(const std::string &Path) {
  return std::filesystem::path(Path).filename().string();
}

static std::string resolvePath(const std::string &BaseDir, const std::string &RelativePath) {
  if (RelativePath.empty())
    return "";
  if (RelativePath[0] == '/')
    return RelativePath;
  
  std::filesystem::path Base(BaseDir);
  std::filesystem::path Rel(RelativePath);
  
  // Remove leading "./" if present
  if (RelativePath.length() >= 2 && RelativePath[0] == '.' && RelativePath[1] == '/') {
    return (Base / RelativePath.substr(2)).string();
  }
  return (Base / RelativePath).string();
}

static std::string shellQuote(const std::string &Text) {
  std::string Quoted = "'";
  for (char C : Text) {
    if (C == '\'')
      Quoted += "'\\''";
    else
      Quoted.push_back(C);
  }
  Quoted += "'";
  return Quoted;
}

static std::string getOutputSubDir(const std::string &CmdPath) {
  std::string ParentDir = getParentDirName(CmdPath);
  if (ParentDir == "llvmir") {
    return "lib";
  } else if (ParentDir == "llvmir-bin") {
    return "bin";
  }
  return ParentDir;  // fallback to parent dir name
}

static std::string normalizePathForCompare(const std::string &Path) {
  return std::filesystem::absolute(std::filesystem::path(Path)).lexically_normal().string();
}

static std::string joinPath(const std::string &Dir, const std::string &Name) {
  return (std::filesystem::path(Dir) / Name).string();
}

static std::vector<std::string> buildLinkArgs(const CmdInfo &Cmd,
                                              const std::map<std::string, std::string> &TempInputPaths,
                                              const std::string &TempOutputPath,
                                              const std::vector<std::string> &ExtraArgs = {}) {
  std::string ClangSuffix = getClangVersionSuffix();
  std::vector<std::string> Args;
  enum class PendingValue { None, Output };
  PendingValue Pending = PendingValue::None;

  for (size_t I = 0; I < Cmd.CommandArgs.size(); ++I) {
    std::string Arg = Cmd.CommandArgs[I];

    if (Pending == PendingValue::Output) {
      Arg = TempOutputPath;
      Pending = PendingValue::None;
    } else if (Arg == "-o" || Arg == "--output") {
      Pending = PendingValue::Output;
    } else if (startsWith(Arg, "--output=")) {
      Arg = "--output=" + TempOutputPath;
    } else if (startsWith(Arg, "-o") && Arg.size() > 2) {
      Arg = "-o" + TempOutputPath;
    } else {
      auto It = TempInputPaths.find(Arg);
      if (It != TempInputPaths.end())
        Arg = It->second;
    }

    if (I == 0)
      Arg = replaceClangArg(Arg, ClangSuffix);

    Args.push_back(Arg);
  }

  Args.insert(Args.end(), ExtraArgs.begin(), ExtraArgs.end());

  return Args;
}

static std::string formatCommandForLog(const std::vector<std::string> &Args) {
  std::string Command;
  for (const auto &Arg : Args) {
    if (!Command.empty())
      Command += " ";
    Command += shellQuote(Arg);
  }
  return Command;
}

//===----------------------------------------------------------------------===//
// Command Execution
//===----------------------------------------------------------------------===//

static bool executeCommand(const std::vector<std::string> &Args,
                           const std::string &WorkingDir) {
  if (Args.empty()) {
    errs() << "Error: Empty command\n";
    return false;
  }

  pid_t Pid = fork();
  if (Pid < 0) {
    errs() << "Error: fork failed\n";
    return false;
  }

  if (Pid == 0) {
    if (chdir(WorkingDir.c_str()) != 0)
      _exit(126);

    std::vector<char *> Argv;
    Argv.reserve(Args.size() + 1);
    for (const auto &Arg : Args)
      Argv.push_back(const_cast<char *>(Arg.c_str()));
    Argv.push_back(nullptr);

    execvp(Argv[0], Argv.data());
    _exit(127);
  }

  int Status = 0;
  if (waitpid(Pid, &Status, 0) < 0) {
    errs() << "Error: waitpid failed\n";
    return false;
  }

  if (WIFEXITED(Status))
    return WEXITSTATUS(Status) == 0;

  if (WIFSIGNALED(Status))
    errs() << "Error: command terminated by signal " << WTERMSIG(Status) << "\n";
  return false;
}

static bool copyOutputFile(const std::string &TempOutputPath,
                           const std::string &FinalOutputDir,
                           const std::string &FinalOutputPath,
                           StringRef Label) {
  std::error_code EC;
  if (!std::filesystem::create_directories(FinalOutputDir, EC) && EC) {
    errs() << "Error creating output directory: " << EC.message() << "\n";
    return false;
  }

  if (access(TempOutputPath.c_str(), F_OK) != 0) {
    errs() << "Error: Output file not found at " << TempOutputPath << "\n";
    return false;
  }

  std::filesystem::copy_file(TempOutputPath, FinalOutputPath,
                             std::filesystem::copy_options::overwrite_existing,
                             EC);
  if (EC) {
    errs() << "Error copying output file: " << EC.message() << "\n";
    return false;
  }

  outs() << Label << ": " << FinalOutputPath << "\n";
  return true;
}

//===----------------------------------------------------------------------===//
// Single _cmd File Processing
//===----------------------------------------------------------------------===//

/// Process a single _cmd file, converting LLVM IR to ELF
/// 
/// \param CmdPath Path to the _cmd script file
/// \param TemplatePathStr Path to template LL file (can be empty)
/// \param OutputDirStr Output directory for generated files
/// \param PgoOutputDirStr Optional output directory for PGO instrumented files
/// \param PgoProfilesOutputDirStr Directory for runtime .profraw files
/// \param KeepTempFlag Whether to keep temporary files after processing
/// \param DryRunFlag If true, only parse and validate without executing
/// \param IncrementalFlag If true, skip if output is newer than input
/// 
/// \return true on success, false on failure
static bool processCmdFile(const std::string &CmdPath,
                           const std::string &TemplatePathStr,
                           const std::string &OutputDirStr,
                           const std::string &PgoOutputDirStr,
                           const std::string &PgoProfilesOutputDirStr,
                           bool KeepTempFlag,
                           bool DryRunFlag = false,
                           bool IncrementalFlag = false) {
  // Step 1: Resolve absolute path of _cmd file
  std::string AbsCmdPath = std::filesystem::absolute(CmdPath).string();
  std::string CmdDir = std::filesystem::path(AbsCmdPath).parent_path().string();
  
  VERBOSE_OUT("Processing _cmd file: " << AbsCmdPath << "\n");
  VERBOSE_OUT("Command directory: " << CmdDir << "\n");
  
  // Step 2: Parse _cmd script to get input/output info
  CmdInfo Cmd = parseCmdScript(AbsCmdPath);
  if (Cmd.InputFiles.empty()) {
    errs() << "Error: Could not find input file in _cmd file: " << AbsCmdPath << "\n";
    return false;
  }
  
  // Validate output file is specified
  if (Cmd.OutputFile.empty()) {
    errs() << "Error: Could not find output file in _cmd file: " << AbsCmdPath << "\n";
    return false;
  }
  
  VERBOSE_OUT("Input file: " << Cmd.InputFile << "\n");
  VERBOSE_OUT("Output file: " << Cmd.OutputFile << "\n");
  if (!Cmd.VersionScript.empty()) {
    VERBOSE_OUT("Version script: " << Cmd.VersionScript << "\n");
  }
  
  // Step 3: Resolve all file paths
  std::vector<std::string> InputIRPaths;
  for (const auto &InputFile : Cmd.InputFiles) {
    std::string InputIRPath = resolvePath(CmdDir, InputFile);
    if (!std::filesystem::exists(InputIRPath)) {
      errs() << "Error: Input IR file does not exist: " << InputIRPath << "\n";
      return false;
    }
    InputIRPaths.push_back(InputIRPath);
  }
  std::string OutputFileName = getFileName(Cmd.OutputFile);
  
  // Step 4: Determine final output directory and check incremental compilation
  std::string OutputSubDir = getOutputSubDir(AbsCmdPath);
  std::string FinalOutputDir = joinPath(OutputDirStr, OutputSubDir);
  std::string FinalOutputPath = joinPath(FinalOutputDir, OutputFileName);
  bool HasPgoOutput = !PgoOutputDirStr.empty();
  std::string PgoFinalOutputDir;
  std::string PgoFinalOutputPath;
  std::string PgoProfilePattern;
  if (HasPgoOutput) {
    PgoFinalOutputDir = joinPath(PgoOutputDirStr, OutputSubDir);
    PgoFinalOutputPath = joinPath(PgoFinalOutputDir, OutputFileName);
    PgoProfilePattern = joinPath(normalizePathForCompare(PgoProfilesOutputDirStr),
                                 OutputFileName + "_%p.profraw");
  }
  
  // Check incremental compilation - skip if output is newer than input
  if (IncrementalFlag && shouldSkipIncremental(InputIRPaths, FinalOutputPath) &&
      (!HasPgoOutput || shouldSkipIncremental(InputIRPaths, PgoFinalOutputPath))) {
    outs() << "Skipping (incremental): " << CmdPath << "\n";
    return true;
  }
  
  // Dry run mode - only validate, don't execute
  if (DryRunFlag) {
    outs() << "Dry run: " << CmdPath << "\n";
    for (const auto &InputIRPath : InputIRPaths) {
      outs() << "  Input: " << InputIRPath << "\n";
    }
    outs() << "  Output: " << FinalOutputPath << "\n";
    if (HasPgoOutput) {
      outs() << "  PGO output: " << PgoFinalOutputPath << "\n";
      outs() << "  PGO profile pattern: " << PgoProfilePattern << "\n";
    }
    if (!Cmd.VersionScript.empty()) {
      outs() << "  Version script: " << Cmd.VersionScript << "\n";
    }
    outs() << "Validation successful.\n";
    return true;
  }
  
  // Step 5: Create temporary directory for processing
  // Use TMPDIR environment variable if set, otherwise default to /tmp
  std::string TempBaseDir = std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp";
  std::string TempDirTemplateStr = TempBaseDir + "/llvmir-XXXXXX";
  // mkdtemp requires a mutable char array, so we copy to a vector
  std::vector<char> TempDirTemplate(TempDirTemplateStr.begin(), TempDirTemplateStr.end());
  TempDirTemplate.push_back('\0');  // null terminator
  char *TempDir = mkdtemp(TempDirTemplate.data());
  if (!TempDir) {
    errs() << "Error: Could not create temporary directory in " << TempBaseDir << "\n";
    return false;
  }
  std::string TempDirStr(TempDir);
  
  std::map<std::string, std::string> TempInputPaths;
  if (!TemplatePathStr.empty()) {
    outs() << "Using template: " << TemplatePathStr << "\n";
  }

  for (size_t I = 0; I < Cmd.InputFiles.size(); ++I) {
    const std::string &InputFile = Cmd.InputFiles[I];
    const std::string &InputIRPath = InputIRPaths[I];
    std::string TempIRPath = TempDirStr + "/input-" + std::to_string(I) + "-" + getFileName(InputFile);

    outs() << "Processing IR: " << InputIRPath << "\n";
    if (!processIR(InputIRPath, TempIRPath, TemplatePathStr)) {
      errs() << "Error: Failed to process IR\n";
      if (!KeepTempFlag) {
        std::filesystem::remove_all(TempDir);
      }
      return false;
    }
    TempInputPaths[InputFile] = TempIRPath;
  }
  
  // Create output subdirectory in temp (for _cmd script's mkdir -p output)
  std::string TempOutputDir = TempDirStr + "/output";
  std::filesystem::create_directories(TempOutputDir);
  
  std::string TempOutputPath = joinPath(TempOutputDir, OutputFileName);

  std::vector<std::string> LinkArgs = buildLinkArgs(Cmd, TempInputPaths, TempOutputPath);
  outs() << "Executing in " << CmdDir << ": " << formatCommandForLog(LinkArgs) << "\n";
  
  if (!executeCommand(LinkArgs, CmdDir)) {
    errs() << "Error: Compilation failed\n";
    if (!KeepTempFlag) {
      std::filesystem::remove_all(TempDir);
    }
    return false;
  }
  if (!copyOutputFile(TempOutputPath, FinalOutputDir, FinalOutputPath, "Output")) {
    if (!KeepTempFlag) {
      std::filesystem::remove_all(TempDir);
    }
    return false;
  }

  if (HasPgoOutput) {
    std::string TempPgoOutputDir = TempDirStr + "/pgo-output";
    std::filesystem::create_directories(TempPgoOutputDir);
    std::string TempPgoOutputPath = joinPath(TempPgoOutputDir, OutputFileName);

    std::error_code EC;
    std::filesystem::create_directories(normalizePathForCompare(PgoProfilesOutputDirStr), EC);
    if (EC) {
      errs() << "Error creating PGO profiles output directory: " << EC.message() << "\n";
      if (!KeepTempFlag) {
        std::filesystem::remove_all(TempDir);
      }
      return false;
    }

    std::map<std::string, std::string> PgoTempInputPaths;
    for (size_t I = 0; I < Cmd.InputFiles.size(); ++I) {
      const std::string &InputFile = Cmd.InputFiles[I];
      const std::string &InputIRPath = InputIRPaths[I];
      std::string TempPgoIRPath = TempDirStr + "/pgo-input-" +
                                  std::to_string(I) + "-" + getFileName(InputFile);

      if (!processIR(InputIRPath, TempPgoIRPath, TemplatePathStr,
                     PgoProfilePattern)) {
        errs() << "Error: Failed to process PGO IR\n";
        if (!KeepTempFlag) {
          std::filesystem::remove_all(TempDir);
        }
        return false;
      }
      PgoTempInputPaths[InputFile] = TempPgoIRPath;
    }

    std::vector<std::string> PgoExtraArgs = {
      "-fprofile-instr-generate=" + PgoProfilePattern
    };
    std::vector<std::string> PgoLinkArgs = buildLinkArgs(Cmd, PgoTempInputPaths,
                                                          TempPgoOutputPath,
                                                          PgoExtraArgs);
    outs() << "Executing PGO in " << CmdDir << ": " << formatCommandForLog(PgoLinkArgs) << "\n";

    if (!executeCommand(PgoLinkArgs, CmdDir)) {
      errs() << "Error: PGO compilation failed\n";
      if (!KeepTempFlag) {
        std::filesystem::remove_all(TempDir);
      }
      return false;
    }

    if (!copyOutputFile(TempPgoOutputPath, PgoFinalOutputDir,
                        PgoFinalOutputPath, "PGO output")) {
      if (!KeepTempFlag) {
        std::filesystem::remove_all(TempDir);
      }
      return false;
    }
  }
  
  // Cleanup
  if (!KeepTempFlag) {
    std::filesystem::remove_all(TempDir);
  }
  
  return true;
}

//===----------------------------------------------------------------------===//
// Main
//===----------------------------------------------------------------------===//

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  for (int I = 1; I < argc; ++I) {
    StringRef Arg(argv[I]);
    if (Arg == "--version" || Arg == "-version") {
      showVersionInfo();
      return 0;
    }
    if (Arg == "--list-targets") {
      showSupportedTargets();
      return 0;
    }
  }
  
  // Show help if no arguments provided
  if (argc == 1) {
    errs() << "OVERVIEW: Convert LLVM IR to ELF executable/shared library\n\n"
           << "USAGE: llvmir-converter [options] <input _cmd file>\n\n"
           << "OPTIONS:\n"
           << "  -o=<directory>   Output directory (default: output)\n"
           << "  --pgo-output=<directory>           PGO instrumented output directory\n"
           << "  --pgo-profiles-output=<directory>  Directory for PGO .profraw output at runtime\n"
           << "  -t=<filename>    Template LL file path\n"
           << "  -v               Enable verbose output\n"
           << "  --dry-run        Parse and validate without executing\n"
           << "  --incremental    Skip files if output is newer than input\n"
           << "  --keep-temp      Keep temporary files\n"
           << "  --version        Show tool version information\n"
           << "  --tool-version   Show tool version information\n"
           << "  --list-targets   List common target architectures\n"
           << "  --help           Display this help message\n";
    return 0;
  }
  
  cl::HideUnrelatedOptions(ConverterCat);
  cl::ParseCommandLineOptions(argc, argv, 
    "Convert LLVM IR to ELF executable/shared library\n");

  // Handle --version flag
  if (ShowVersion) {
    showVersionInfo();
    return 0;
  }

  if (!PgoOutputDir.empty() && PgoProfilesOutputDir.empty()) {
    errs() << "Error: --pgo-output requires --pgo-profiles-output=<directory>\n";
    return 1;
  }

  if (!PgoOutputDir.empty() &&
      normalizePathForCompare(std::string(OutputDir)) == normalizePathForCompare(std::string(PgoOutputDir))) {
    errs() << "Error: --pgo-output must be different from -o output directory\n";
    return 1;
  }
  
  // Get absolute path of input file
  std::string AbsInputPath = std::filesystem::absolute(std::string(InputCmdFile)).string();
  
  // Process single _cmd file with all flags
  if (!processCmdFile(AbsInputPath, std::string(TemplatePath), 
                      std::string(OutputDir), std::string(PgoOutputDir),
                      std::string(PgoProfilesOutputDir), KeepTemp, DryRun,
                      Incremental)) {
    return 1;
  }
  
  return 0;
}
