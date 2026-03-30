#include "workspace_builder.hh"
#include "lona/abi/abi.hh"
#include "lona/abi/native_abi.hh"
#include "lona/err/err.hh"
#include "lona/resolve/resolve.hh"
#include "lona/sema/hir.hh"
#include "lona/util/time.hh"
#include "lona/visitor.hh"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm-18/llvm/IR/LLVMContext.h>
#include <llvm-18/llvm/IR/Module.h>
#include <llvm-18/llvm/IR/PassManager.h>
#include <llvm-18/llvm/IR/Verifier.h>
#include <llvm-18/llvm/Passes/OptimizationLevel.h>
#include <llvm-18/llvm/Passes/PassBuilder.h>
#include <llvm-18/llvm/Target/TargetMachine.h>
#include <optional>
#include <iomanip>
#include <sstream>
#include <utility>

namespace lona {
namespace workspace_builder_impl {

llvm::OptimizationLevel
getOptimizationLevel(int optLevel) {
    switch (optLevel) {
    case 0:
        return llvm::OptimizationLevel::O0;
    case 1:
        return llvm::OptimizationLevel::O1;
    case 2:
        return llvm::OptimizationLevel::O2;
    case 3:
        return llvm::OptimizationLevel::O3;
    default:
        return llvm::OptimizationLevel::O0;
    }
}

void
optimizeModule(llvm::Module &module, int optLevel) {
    if (optLevel <= 0) {
        return;
    }

    llvm::LoopAnalysisManager loopAM;
    llvm::FunctionAnalysisManager functionAM;
    llvm::CGSCCAnalysisManager cgsccAM;
    llvm::ModuleAnalysisManager moduleAM;
    llvm::PassBuilder passBuilder;

    passBuilder.registerLoopAnalyses(loopAM);
    passBuilder.registerFunctionAnalyses(functionAM);
    passBuilder.registerCGSCCAnalyses(cgsccAM);
    passBuilder.registerModuleAnalyses(moduleAM);
    passBuilder.crossRegisterProxies(loopAM, functionAM, cgsccAM, moduleAM);

    llvm::ModulePassManager modulePasses =
        passBuilder.buildPerModuleDefaultPipeline(getOptimizationLevel(optLevel));
    modulePasses.run(module, moduleAM);
}

bool
verifyCompiledModule(llvm::Module &module, std::ostream &out) {
    std::string verifyErrors;
    llvm::raw_string_ostream verifyOut(verifyErrors);
    if (!llvm::verifyModule(module, &verifyOut)) {
        return true;
    }
    verifyOut.flush();
    out << verifyErrors;
    return false;
}

std::unique_ptr<llvm::Module>
parseArtifactBitcodeModule(const ModuleArtifact &artifact, llvm::LLVMContext &context) {
    llvm::StringRef bytes(
        reinterpret_cast<const char *>(artifact.bitcode().data()),
        artifact.bitcode().size());
    auto buffer = llvm::MemoryBuffer::getMemBufferCopy(
        bytes, toStdString(artifact.path() + ".bc"));
    auto module = llvm::parseBitcodeFile(buffer->getMemBufferRef(), context);
    if (module) {
        return std::move(*module);
    }

    throw DiagnosticError(DiagnosticError::Category::Internal,
                          "failed to parse cached LLVM bitcode for module `" +
                              toStdString(artifact.path()) + "`",
                          llvm::toString(module.takeError()));
}

llvm::StringRef
languageEntryName() {
    return "__lona_main__";
}

llvm::StringRef
hostedArgcName() {
    return "__lona_argc";
}

llvm::StringRef
hostedArgvName() {
    return "__lona_argv";
}

bool
isLanguageEntryType(llvm::FunctionType *funcType) {
    return funcType && funcType->getNumParams() == 0 &&
        funcType->getReturnType()->isIntegerTy(32);
}

void
linkSyntheticModule(llvm::Linker &linker, std::unique_ptr<llvm::Module> module,
                    const std::string &context) {
    if (!module) {
        return;
    }
    if (linker.linkInModule(std::move(module))) {
        throw DiagnosticError(
            DiagnosticError::Category::Internal,
            "failed to link synthetic " + context + " module",
            "Check for duplicate entry symbols or incompatible LLVM module state.");
    }
}

bool
moduleUsesNativeAbi(const llvm::Module &module) {
    for (const auto &func : module) {
        auto abi = functionAbiAnnotation(func);
        if (abi && *abi == AbiKind::Native) {
            return true;
        }
    }
    return false;
}

std::optional<std::string>
nativeAbiMarkerSectionFor(llvm::StringRef targetTriple) {
    llvm::Triple triple(normalizeTargetTriple(targetTriple.str()));
    if (triple.isOSBinFormatELF()) {
        return std::string(".lona.native_abi");
    }
    if (triple.isOSBinFormatMachO()) {
        return std::string("__TEXT,__lona_abi");
    }
    return std::nullopt;
}

void
ensureNativeAbiVersionField(llvm::Module &module, llvm::StringRef targetTriple) {
    if (!moduleUsesNativeAbi(module)) {
        return;
    }

    const auto symbolName = lonaNativeAbiVersionSymbolName();
    if (module.getGlobalVariable(symbolName)) {
        return;
    }

    auto &context = module.getContext();
    auto payload = lonaNativeAbiVersionPayload();
    auto *init = llvm::ConstantDataArray::getString(context, payload, true);
    auto *field = new llvm::GlobalVariable(
        module, init->getType(), true, llvm::GlobalValue::InternalLinkage, init,
        symbolName);
    if (auto section = nativeAbiMarkerSectionFor(targetTriple)) {
        field->setSection(*section);
    }
    field->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    field->setAlignment(llvm::Align(1));
    llvm::appendToCompilerUsed(module, {field});
}

bool
moduleHasFunctionSymbol(const llvm::Module &module, llvm::StringRef name) {
    return module.getFunction(name) != nullptr;
}

llvm::GlobalVariable *
getOrCreateHostedArgcGlobal(llvm::Module &module) {
    if (auto *existing = module.getGlobalVariable(hostedArgcName())) {
        return existing;
    }

    auto &context = module.getContext();
    return new llvm::GlobalVariable(module, llvm::Type::getInt32Ty(context), false,
                                    llvm::GlobalValue::ExternalLinkage,
                                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0),
                                    hostedArgcName());
}

llvm::GlobalVariable *
getOrCreateHostedArgvGlobal(llvm::Module &module) {
    if (auto *existing = module.getGlobalVariable(hostedArgvName())) {
        return existing;
    }

    auto &context = module.getContext();
    auto *ptrTy = llvm::PointerType::getUnqual(context);
    return new llvm::GlobalVariable(module, ptrTy, false,
                                    llvm::GlobalValue::ExternalLinkage,
                                    llvm::ConstantPointerNull::get(ptrTy),
                                    hostedArgvName());
}

std::unique_ptr<llvm::Module>
createHostedMainShimModule(llvm::LLVMContext &context,
                           llvm::StringRef targetTriple) {
    auto module = std::make_unique<llvm::Module>("lona.hosted_entry_shim", context);
    configureModuleTargetLayout(*module, targetTriple);

    auto *entryType =
        llvm::FunctionType::get(llvm::Type::getInt32Ty(context), {}, false);
    auto *entryDecl = llvm::Function::Create(
        entryType, llvm::Function::ExternalLinkage, languageEntryName(), *module);
    annotateFunctionAbi(*entryDecl, AbiKind::Native);

    auto *argcGlobal = getOrCreateHostedArgcGlobal(*module);
    auto *argvGlobal = getOrCreateHostedArgvGlobal(*module);

    auto *mainType = llvm::FunctionType::get(
        llvm::Type::getInt32Ty(context),
        {llvm::Type::getInt32Ty(context), llvm::PointerType::getUnqual(context)},
        false);
    auto *wrapper = llvm::Function::Create(
        mainType, llvm::Function::ExternalLinkage, "main", *module);
    annotateFunctionAbi(*wrapper, AbiKind::C);

    auto *block = llvm::BasicBlock::Create(context, "entry", wrapper);
    llvm::IRBuilder<> builder(block);
    auto argIt = wrapper->arg_begin();
    llvm::Value *argcValue = &*argIt++;
    llvm::Value *argvValue = &*argIt;
    builder.CreateStore(argcValue, argcGlobal);
    builder.CreateStore(argvValue, argvGlobal);
    builder.CreateRet(builder.CreateCall(entryDecl));
    return module;
}

void
emitObjectFile(llvm::Module &module, llvm::StringRef targetTriple,
               std::ostream &out) {
    llvm::SmallString<0> objectData;
    llvm::raw_svector_ostream objectOut(objectData);
    llvm::legacy::PassManager passManager;
    auto &targetMachine = targetMachineFor(targetTriple);

    if (targetMachine.addPassesToEmitFile(passManager, objectOut, nullptr,
                                          llvm::CodeGenFileType::ObjectFile)) {
        throw DiagnosticError(DiagnosticError::Category::Internal,
                              "LLVM target machine cannot emit object files for the active target",
                              "Check native target initialization and object emission setup.");
    }

    passManager.run(module);
    out.write(objectData.data(), static_cast<std::streamsize>(objectData.size()));
    if (!out) {
        throw DiagnosticError(DiagnosticError::Category::Driver,
                              "I couldn't write the emitted object file.",
                              "Check that the destination stream or file is writable.");
    }
}

void
emitObjectFile(llvm::Module &module, llvm::StringRef targetTriple,
               const std::string &outputPath) {
    std::error_code error;
    llvm::raw_fd_ostream out(outputPath, error, llvm::sys::fs::OF_None);
    if (error) {
        throw DiagnosticError(
            DiagnosticError::Category::Driver,
            "I couldn't open output file `" + outputPath + "`.",
            "Check that the path is writable and that parent directories exist.");
    }

    llvm::legacy::PassManager passManager;
    auto &targetMachine = targetMachineFor(targetTriple);
    if (targetMachine.addPassesToEmitFile(passManager, out, nullptr,
                                          llvm::CodeGenFileType::ObjectFile)) {
        throw DiagnosticError(DiagnosticError::Category::Internal,
                              "LLVM target machine cannot emit object files for the active target",
                              "Check native target initialization and object emission setup.");
    }

    passManager.run(module);
    out.flush();
    if (out.has_error()) {
        throw DiagnosticError(
            DiagnosticError::Category::Driver,
            "I couldn't write output file `" + outputPath + "`.",
            "Check that the path is writable and that the filesystem has enough space.");
    }
}

ModuleArtifact::ByteBuffer
emitObjectData(llvm::Module &module, llvm::StringRef targetTriple) {
    llvm::SmallString<0> objectData;
    llvm::raw_svector_ostream objectOut(objectData);
    llvm::legacy::PassManager passManager;
    auto &targetMachine = targetMachineFor(targetTriple);

    if (targetMachine.addPassesToEmitFile(passManager, objectOut, nullptr,
                                          llvm::CodeGenFileType::ObjectFile)) {
        throw DiagnosticError(DiagnosticError::Category::Internal,
                              "LLVM target machine cannot emit object files for the active target",
                              "Check native target initialization and object emission setup.");
    }

    passManager.run(module);
    return ModuleArtifact::ByteBuffer(objectData.begin(), objectData.end());
}

ModuleArtifact::ByteBuffer
emitBitcodeData(llvm::Module &module) {
    llvm::SmallVector<char, 0> bitcodeData;
    llvm::raw_svector_ostream bitcodeOut(bitcodeData);
    llvm::WriteBitcodeToFile(module, bitcodeOut);
    return ModuleArtifact::ByteBuffer(bitcodeData.begin(), bitcodeData.end());
}

void
writeBinaryFile(const std::filesystem::path &path,
                const ModuleArtifact::ByteBuffer &bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::out);
    if (!out) {
        throw DiagnosticError(DiagnosticError::Category::Driver,
                              "I couldn't open output file `" + path.string() + "`.",
                              "Check that the path is writable and that parent directories exist.");
    }
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        throw DiagnosticError(DiagnosticError::Category::Driver,
                              "I couldn't write output file `" + path.string() + "`.",
                              "Check that the path is writable and that the filesystem has enough space.");
    }
}

std::optional<ModuleArtifact::ByteBuffer>
readBinaryFileIfPresent(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary | std::ios::in);
    if (!in) {
        return std::nullopt;
    }

    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    if (size < 0) {
        throw DiagnosticError(DiagnosticError::Category::Driver,
                              "I couldn't inspect cached object file `" +
                                  path.string() + "`.",
                              "Check that the cache directory is readable.");
    }
    in.seekg(0, std::ios::beg);

    ModuleArtifact::ByteBuffer bytes(static_cast<std::size_t>(size));
    if (size > 0) {
        in.read(reinterpret_cast<char *>(bytes.data()), size);
    }
    if (!in) {
        throw DiagnosticError(DiagnosticError::Category::Driver,
                              "I couldn't read cached object file `" +
                                  path.string() + "`.",
                              "Check that the cache directory is readable.");
    }
    return bytes;
}

void
appendHIRFunctions(HIRModule &target, const HIRModule &source) {
    for (auto *func : source.getFunctions()) {
        target.addFunction(func);
    }
}

void
accumulateArtifactEmit(SessionStats &stats, double elapsedMs) {
    stats.artifactEmitMs += elapsedMs;
    stats.codegenMs += elapsedMs;
}

void
accumulateOutputEmit(SessionStats &stats, double renderMs, double writeMs) {
    stats.outputRenderMs += renderMs;
    stats.outputWriteMs += writeMs;
    stats.outputEmitMs += renderMs + writeMs;
    stats.codegenMs += renderMs + writeMs;
}

}  // namespace workspace_builder_impl

using workspace_builder_impl::appendHIRFunctions;
using workspace_builder_impl::accumulateArtifactEmit;
using workspace_builder_impl::accumulateOutputEmit;
using workspace_builder_impl::createHostedMainShimModule;
using workspace_builder_impl::emitBitcodeData;
using workspace_builder_impl::emitObjectData;
using workspace_builder_impl::emitObjectFile;
using workspace_builder_impl::ensureNativeAbiVersionField;
using workspace_builder_impl::isLanguageEntryType;
using workspace_builder_impl::languageEntryName;
using workspace_builder_impl::linkSyntheticModule;
using workspace_builder_impl::moduleHasFunctionSymbol;
using workspace_builder_impl::moduleUsesNativeAbi;
using workspace_builder_impl::optimizeModule;
using workspace_builder_impl::parseArtifactBitcodeModule;
using workspace_builder_impl::readBinaryFileIfPresent;
using workspace_builder_impl::verifyCompiledModule;
using workspace_builder_impl::writeBinaryFile;

WorkspaceBuilder::WorkspaceBuilder(CompilerWorkspace &workspace,
                                   const WorkspaceLoader &loader)
    : workspace_(workspace),
      loader_(loader),
      executor_(createSerialModuleExecutor()) {
    pipeline_.addStage("collect-declarations", [this](IRPipelineContext &context) {
        auto start = Clock::now();
        const bool exportEntryNamespace =
            context.rootUnit && context.rootUnit->path() != context.entryUnit.path();
        auto dependencyStart = Clock::now();
        for (const auto &dependencyPath :
             context.moduleGraph.dependenciesOf(context.entryUnit.path())) {
            auto *loadedUnit = workspace_.moduleGraph().find(dependencyPath);
            if (loadedUnit == nullptr) {
                throw DiagnosticError(DiagnosticError::Category::Internal,
                                      "module graph dependency references a missing unit",
                                      "This looks like a compiler module graph bug.");
            }
            loader_.validateImportedUnit(*loadedUnit);
            collectUnitDeclarations(&context.build.global, *loadedUnit, true);
        }
        context.stats.dependencyDeclarationMs +=
            elapsedMillis(dependencyStart, Clock::now());
        auto entryStart = Clock::now();
        collectUnitDeclarations(&context.build.global, context.entryUnit,
                                exportEntryNamespace);
        context.stats.entryDeclarationMs += elapsedMillis(entryStart, Clock::now());
        context.stats.declarationMs += elapsedMillis(start, Clock::now());
        return 0;
    });

    pipeline_.addStage("define-globals", [](IRPipelineContext &context) {
        defineUnitGlobals(&context.build.global, context.entryUnit);
        return 0;
    });

    pipeline_.addStage("lower-hir", [](IRPipelineContext &context) {
        auto start = Clock::now();
        auto resolveStart = Clock::now();
        auto resolved = resolveModule(&context.build.global, context.entryUnit.syntaxTree(),
                                      &context.entryUnit);
        context.stats.resolveMs += elapsedMillis(resolveStart, Clock::now());
        auto analyzeStart = Clock::now();
        auto hirModule =
            analyzeModule(&context.build.global, *resolved, &context.entryUnit);
        context.stats.analyzeMs += elapsedMillis(analyzeStart, Clock::now());
        appendHIRFunctions(context.programHIR, *hirModule);
        context.loweredModules.push_back(std::move(hirModule));
        context.stats.lowerMs += elapsedMillis(start, Clock::now());
        return 0;
    });

    pipeline_.addStage("emit-llvm", [](IRPipelineContext &context) {
        auto start = Clock::now();
        emitHIRModule(&context.build.global, &context.programHIR,
                      context.options.debugInfo,
                      toStdString(context.entryUnit.path()));
        auto emitMs = elapsedMillis(start, Clock::now());
        context.stats.emitLlvmMs += emitMs;
        context.stats.codegenMs += emitMs;
        return 0;
    });

    pipeline_.addStage("optimize-llvm", [](IRPipelineContext &context) {
        auto start = Clock::now();
        optimizeModule(context.build.module, context.options.optLevel);
        auto optimizeMs = elapsedMillis(start, Clock::now());
        context.stats.moduleOptimizeMs += optimizeMs;
        context.stats.optimizeMs += optimizeMs;
        return 0;
    });

    pipeline_.addStage("verify-llvm", [](IRPipelineContext &context) {
        if (!context.options.verifyIR) {
            return 0;
        }
        auto start = Clock::now();
        const bool verifyOk = verifyCompiledModule(context.build.module, context.out);
        auto verifyMs = elapsedMillis(start, Clock::now());
        context.stats.moduleVerifyMs += verifyMs;
        context.stats.verifyMs += verifyMs;
        return verifyOk ? 0 : 1;
    });

    pipeline_.addStage("print-llvm", [](IRPipelineContext &context) {
        if (!context.captureIRText) {
            return 0;
        }
        std::string ir;
        llvm::raw_string_ostream irOut(ir);
        context.build.module.print(irOut, nullptr);
        irOut.flush();
        context.out << ir;
        return 0;
    });
}

std::unordered_map<string, std::uint64_t>
WorkspaceBuilder::collectDependencyInterfaceHashes(const CompilationUnit &unit) const {
    std::unordered_map<string, std::uint64_t> hashes;
    for (const auto &dependencyPath : workspace_.moduleGraph().dependenciesOf(unit.path())) {
        auto *dependency = workspace_.moduleGraph().find(dependencyPath);
        if (dependency == nullptr) {
            throw DiagnosticError(DiagnosticError::Category::Internal,
                                  "module graph dependency references a missing unit",
                                  "This looks like a compiler module graph bug.");
        }
        hashes.emplace(dependency->moduleKey(), dependency->interfaceHash());
    }
    return hashes;
}

std::string
WorkspaceBuilder::bundleObjectFileName(const ModuleArtifact &artifact) const {
    std::string stem =
        artifact.moduleName().empty() ? "module" : toStdString(artifact.moduleName());
    for (char &ch : stem) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (!(std::isalnum(byte) || ch == '_' || ch == '-')) {
            ch = '_';
        }
    }

    std::vector<std::pair<string, std::uint64_t>> dependencies(
        artifact.dependencyInterfaceHashes().begin(),
        artifact.dependencyInterfaceHashes().end());
    std::sort(dependencies.begin(), dependencies.end(),
              [](const auto &lhs, const auto &rhs) {
                  return lhs.first < rhs.first;
              });

    std::ostringstream cacheKey;
    cacheKey << artifact.moduleKey() << "|" << artifact.targetTriple() << "|O"
             << artifact.optLevel() << "|"
             << (artifact.debugInfo() ? "g" : "ng") << "|src="
             << artifact.sourceHash() << "|iface=" << artifact.interfaceHash()
             << "|impl=" << artifact.implementationHash();
    for (const auto &[dependencyKey, dependencyHash] : dependencies) {
        cacheKey << "|dep=" << dependencyKey << ":" << dependencyHash;
    }

    std::ostringstream suffix;
    suffix << std::hex << std::setw(16) << std::setfill('0')
           << static_cast<unsigned long long>(
                  std::hash<std::string>{}(cacheKey.str()));
    return stem + "-" + suffix.str() + ".o";
}

std::filesystem::path
WorkspaceBuilder::bundleObjectPath(
    const ModuleArtifact &artifact,
    const std::filesystem::path &bundleDir) const {
    return bundleDir / bundleObjectFileName(artifact);
}

bool
WorkspaceBuilder::matchesArtifact(const CompilationUnit &unit,
                                  const ModuleArtifact &artifact,
                                  const CompileOptions &options) const {
    if (artifact.sourceHash() != unit.sourceHash() ||
        artifact.interfaceHash() != unit.interfaceHash() ||
        artifact.implementationHash() != unit.implementationHash()) {
        return false;
    }
    if (toStdString(artifact.targetTriple()) !=
            normalizeTargetTriple(options.targetTriple) ||
        artifact.optLevel() != options.optLevel ||
        artifact.debugInfo() != options.debugInfo) {
        return false;
    }
    return artifact.dependencyInterfaceHashes() ==
        collectDependencyInterfaceHashes(unit);
}

ModuleArtifact *
WorkspaceBuilder::reusableArtifactFor(const CompilationUnit &unit,
                                      const CompileOptions &options) const {
    if (options.noCache) {
        return nullptr;
    }
    auto *artifact = workspace_.findArtifact(unit.path());
    if (artifact == nullptr) {
        return nullptr;
    }
    return matchesArtifact(unit, *artifact, options) ? artifact : nullptr;
}

ModuleArtifact
WorkspaceBuilder::createArtifact(const CompilationUnit &unit,
                                 const CompileOptions &options) const {
    ModuleArtifact artifact(unit.path(), unit.moduleKey(), unit.moduleName(),
                            unit.sourceHash(), unit.interfaceHash(),
                            unit.implementationHash());
    artifact.setDependencyInterfaceHashes(collectDependencyInterfaceHashes(unit));
    artifact.setCompileProfile(normalizeTargetTriple(options.targetTriple),
                               options.optLevel, options.debugInfo);
    return artifact;
}

int
WorkspaceBuilder::ensureArtifactOutputs(ModuleArtifact &artifact,
                                        const CompileOptions &options,
                                        bool requireObjects, bool requireBitcode,
                                        SessionStats &stats) const {
    if (requireObjects && artifact.hasObjectCode()) {
        ++stats.reusedModuleObjects;
        requireObjects = false;
    }
    if (requireBitcode && artifact.hasBitcode()) {
        ++stats.reusedModuleBitcode;
        requireBitcode = false;
    }
    if (!requireObjects && !requireBitcode) {
        return 0;
    }

    auto restoreStart = Clock::now();
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = parseArtifactBitcodeModule(artifact, *context);
    stats.cacheRestoreMs += elapsedMillis(restoreStart, Clock::now());
    artifact.setContainsNativeAbi(moduleUsesNativeAbi(*module));
    if (requireBitcode) {
        auto emitStart = Clock::now();
        artifact.setBitcode(emitBitcodeData(*module));
        accumulateArtifactEmit(stats, elapsedMillis(emitStart, Clock::now()));
        ++stats.emittedModuleBitcode;
    }
    if (requireObjects) {
        ensureNativeAbiVersionField(*module, options.targetTriple);
        auto emitStart = Clock::now();
        artifact.setObjectCode(emitObjectData(*module, options.targetTriple));
        accumulateArtifactEmit(stats, elapsedMillis(emitStart, Clock::now()));
        ++stats.emittedModuleObjects;
    }
    return 0;
}

int
WorkspaceBuilder::buildArtifacts(CompilationUnit &rootUnit,
                                 const CompileOptions &options,
                                 bool requireObjects, bool requireBitcode,
                                 const std::filesystem::path *objectCacheDir,
                                 SessionStats &stats,
                                 std::ostream &out) const {
    workspace_.buildQueue().reset(workspace_.moduleGraph(), rootUnit.path());
    return executor_->execute(workspace_.buildQueue(), [&](const string &path) -> int {
        auto *queuedUnit = workspace_.moduleGraph().find(path);
        if (queuedUnit == nullptr) {
            throw DiagnosticError(DiagnosticError::Category::Internal,
                                  "module build queue references a missing unit",
                                      "This looks like a compiler module queue bug.");
        }

        auto cacheLookupStart = Clock::now();
        auto *cachedArtifact = reusableArtifactFor(*queuedUnit, options);
        stats.cacheLookupMs += elapsedMillis(cacheLookupStart, Clock::now());
        if (cachedArtifact != nullptr && requireBitcode && !cachedArtifact->hasBitcode()) {
            cachedArtifact = nullptr;
        }
        if (cachedArtifact != nullptr && requireObjects && !cachedArtifact->hasObjectCode() &&
            !cachedArtifact->hasBitcode()) {
            cachedArtifact = nullptr;
        }
        if (cachedArtifact != nullptr) {
            queuedUnit->markCompiled();
            ++stats.reusedModules;
            int artifactExitCode = ensureArtifactOutputs(
                *cachedArtifact, options, requireObjects, requireBitcode, stats);
            if (artifactExitCode != 0) {
                return artifactExitCode;
            }
            return 0;
        }

        ModuleArtifact artifact = createArtifact(*queuedUnit, options);
        if (!options.noCache && objectCacheDir != nullptr && requireObjects &&
            !requireBitcode) {
            auto cacheRestoreStart = Clock::now();
            auto cachedObject = readBinaryFileIfPresent(
                bundleObjectPath(artifact, *objectCacheDir));
            stats.cacheRestoreMs += elapsedMillis(cacheRestoreStart, Clock::now());
            if (cachedObject.has_value()) {
                artifact.setObjectCode(std::move(*cachedObject));
                workspace_.storeArtifact(std::move(artifact));
                queuedUnit->markCompiled();
                ++stats.reusedModules;
                ++stats.reusedModuleObjects;
                return 0;
            }
        }
        int moduleExitCode =
            compileModule(*queuedUnit, options, artifact, requireObjects,
                          requireBitcode, stats, out);
        if (moduleExitCode != 0) {
            return moduleExitCode;
        }
        workspace_.storeArtifact(std::move(artifact));
        return 0;
    });
}

int
WorkspaceBuilder::compileModule(CompilationUnit &unit, const CompileOptions &options,
                                ModuleArtifact &artifact, bool emitObject,
                                bool emitBitcode,
                                SessionStats &stats, std::ostream &out) const {
    unit.clearResolvedTypes();
    std::ostringstream ir;
    IRPipelineContext context(unit, workspace_.moduleGraph(), options, ir, stats);
    context.rootUnit = workspace_.moduleGraph().root();
    context.captureIRText = false;
    int exitCode = pipeline_.run(context);
    if (exitCode == 0) {
        unit.markCompiled();
        artifact.setContainsNativeAbi(moduleUsesNativeAbi(context.build.module));
        if (emitBitcode) {
            auto emitStart = Clock::now();
            artifact.setBitcode(emitBitcodeData(context.build.module));
            accumulateArtifactEmit(stats, elapsedMillis(emitStart, Clock::now()));
            ++stats.emittedModuleBitcode;
        }
        if (emitObject) {
            ensureNativeAbiVersionField(context.build.module, options.targetTriple);
            auto emitStart = Clock::now();
            artifact.setObjectCode(
                emitObjectData(context.build.module, options.targetTriple));
            accumulateArtifactEmit(stats, elapsedMillis(emitStart, Clock::now()));
            ++stats.emittedModuleObjects;
        }
        ++stats.compiledModules;
    } else {
        out << ir.str();
    }
    return exitCode;
}

WorkspaceBuilder::LinkedModule
WorkspaceBuilder::linkArtifacts(const CompilationUnit &rootUnit,
                                bool hostedEntry, bool verifyIR,
                                SessionStats &stats,
                                std::ostream &out) const {
    auto *rootArtifact = workspace_.findArtifact(rootUnit.path());
    if (rootArtifact == nullptr) {
        throw DiagnosticError(DiagnosticError::Category::Internal,
                              "root module artifact was not produced",
                              "This looks like a compiler module scheduling bug.");
    }

    double linkLoadMs = 0.0;
    double linkMergeMs = 0.0;
    auto context = std::make_unique<llvm::LLVMContext>();
    auto loadStart = Clock::now();
    auto linkedModule = parseArtifactBitcodeModule(*rootArtifact, *context);
    linkLoadMs += elapsedMillis(loadStart, Clock::now());
    llvm::Linker linker(*linkedModule);
    for (const auto &path : workspace_.moduleGraph().postOrderFrom(rootUnit.path())) {
        if (path == rootUnit.path()) {
            continue;
        }
        auto *artifact = workspace_.findArtifact(path);
        if (artifact == nullptr) {
            throw DiagnosticError(DiagnosticError::Category::Internal,
                                  "linked module is missing dependency artifact `" +
                                      toStdString(path) + "`",
                                  "This looks like a compiler module scheduling bug.");
        }
        loadStart = Clock::now();
        auto dependencyModule = parseArtifactBitcodeModule(*artifact, *context);
        linkLoadMs += elapsedMillis(loadStart, Clock::now());
        auto mergeStart = Clock::now();
        if (linker.linkInModule(std::move(dependencyModule))) {
            throw DiagnosticError(
                DiagnosticError::Category::Internal,
                "failed to link module `" + toStdString(artifact->path()) +
                    "` into root module `" + toStdString(rootUnit.path()) + "`",
                "Check for duplicate IR symbols or incompatible LLVM module state.");
        }
        linkMergeMs += elapsedMillis(mergeStart, Clock::now());
    }

    const bool hasLanguageEntry = moduleHasFunctionSymbol(*linkedModule, languageEntryName());
    if (hasLanguageEntry && hostedEntry && !moduleHasFunctionSymbol(*linkedModule, "main")) {
        auto mergeStart = Clock::now();
        linkSyntheticModule(
            linker,
            createHostedMainShimModule(*context, linkedModule->getTargetTriple()),
            "hosted entry shim");
        linkMergeMs += elapsedMillis(mergeStart, Clock::now());
    }

    if (verifyIR) {
        auto verifyStart = Clock::now();
        const bool ok = verifyCompiledModule(*linkedModule, out);
        auto verifyMs = elapsedMillis(verifyStart, Clock::now());
        stats.linkVerifyMs += verifyMs;
        stats.verifyMs += verifyMs;
        if (!ok) {
            return {};
        }
    }
    stats.linkLoadMs += linkLoadMs;
    stats.linkMergeMs += linkMergeMs;
    stats.linkMs += linkLoadMs + linkMergeMs;
    return {std::move(context), std::move(linkedModule)};
}

std::size_t
WorkspaceBuilder::loadedUnitCount() const {
    auto *root = workspace_.moduleGraph().root();
    return root ? workspace_.moduleGraph().postOrderFrom(root->path()).size() : 0;
}

int
WorkspaceBuilder::emitHostedEntryObject(const CompileOptions &options,
                                        const std::string &outputPath,
                                        SessionStats &stats,
                                        std::ostream &out) const {
    if (!targetUsesHostedEntry(options.targetTriple)) {
        throw DiagnosticError(
            DiagnosticError::Category::Driver,
            "`--emit entry` is only supported for hosted targets",
            "Use a hosted target triple such as `x86_64-unknown-linux-gnu`.");
    }

    auto context = std::make_unique<llvm::LLVMContext>();
    auto hostedShim =
        createHostedMainShimModule(*context, normalizeTargetTriple(options.targetTriple));
    if (options.verifyIR) {
        auto verifyStart = Clock::now();
        const bool ok = verifyCompiledModule(*hostedShim, out);
        auto verifyMs = elapsedMillis(verifyStart, Clock::now());
        stats.moduleVerifyMs += verifyMs;
        stats.verifyMs += verifyMs;
        if (!ok) {
            return 1;
        }
    }
    ensureNativeAbiVersionField(*hostedShim, options.targetTriple);
    auto emitStart = Clock::now();
    if (outputPath.empty()) {
        emitObjectFile(*hostedShim, options.targetTriple, out);
    } else {
        emitObjectFile(*hostedShim, options.targetTriple, outputPath);
    }
    accumulateOutputEmit(stats, elapsedMillis(emitStart, Clock::now()), 0.0);
    return 0;
}

int
WorkspaceBuilder::emitIR(CompilationUnit &rootUnit, const CompileOptions &options,
                         SessionStats &stats, std::ostream &out) const {
    const bool useLTO = options.ltoMode == CompileOptions::LTOMode::Full;
    int exitCode =
        buildArtifacts(rootUnit, options, false, true, nullptr, stats, out);
    if (exitCode != 0) {
        return exitCode;
    }

    auto linked = linkArtifacts(rootUnit,
                                targetUsesHostedEntry(options.targetTriple),
                                options.verifyIR, stats, out);
    if (!linked.module) {
        return 1;
    }
    if (useLTO) {
        auto optimizeStart = Clock::now();
        optimizeModule(*linked.module, options.optLevel);
        auto optimizeMs = elapsedMillis(optimizeStart, Clock::now());
        stats.ltoOptimizeMs += optimizeMs;
        stats.optimizeMs += optimizeMs;
        if (options.verifyIR) {
            auto verifyStart = Clock::now();
            const bool ok = verifyCompiledModule(*linked.module, out);
            auto verifyMs = elapsedMillis(verifyStart, Clock::now());
            stats.linkVerifyMs += verifyMs;
            stats.verifyMs += verifyMs;
            if (!ok) {
                return 1;
            }
        }
    }

    auto emitStart = Clock::now();
    llvm::raw_os_ostream irOut(out);
    linked.module->print(irOut, nullptr);
    irOut.flush();
    accumulateOutputEmit(stats, elapsedMillis(emitStart, Clock::now()), 0.0);
    return 0;
}

int
WorkspaceBuilder::emitObject(CompilationUnit &rootUnit,
                             const CompileOptions &options,
                             const std::string &outputPath,
                             SessionStats &stats, std::ostream &out) const {
    const bool useLTO = options.ltoMode == CompileOptions::LTOMode::Full;
    int exitCode =
        buildArtifacts(rootUnit, options, false, true, nullptr, stats, out);
    if (exitCode != 0) {
        return exitCode;
    }

    auto linked = linkArtifacts(rootUnit,
                                targetUsesHostedEntry(options.targetTriple),
                                options.verifyIR, stats, out);
    if (!linked.module) {
        return 1;
    }
    if (useLTO) {
        auto optimizeStart = Clock::now();
        optimizeModule(*linked.module, options.optLevel);
        auto optimizeMs = elapsedMillis(optimizeStart, Clock::now());
        stats.ltoOptimizeMs += optimizeMs;
        stats.optimizeMs += optimizeMs;
        if (options.verifyIR) {
            auto verifyStart = Clock::now();
            const bool ok = verifyCompiledModule(*linked.module, out);
            auto verifyMs = elapsedMillis(verifyStart, Clock::now());
            stats.linkVerifyMs += verifyMs;
            stats.verifyMs += verifyMs;
            if (!ok) {
                return 1;
            }
        }
    }

    ensureNativeAbiVersionField(*linked.module, options.targetTriple);
    if (!outputPath.empty()) {
        auto emitStart = Clock::now();
        emitObjectFile(*linked.module, options.targetTriple, outputPath);
        accumulateOutputEmit(stats, elapsedMillis(emitStart, Clock::now()), 0.0);
        return 0;
    }

    auto renderStart = Clock::now();
    auto bytes = emitObjectData(*linked.module, options.targetTriple);
    auto renderMs = elapsedMillis(renderStart, Clock::now());
    auto writeStart = Clock::now();
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        throw DiagnosticError(DiagnosticError::Category::Driver,
                              "I couldn't write the emitted object file.",
                              "Check that the destination stream or file is writable.");
    }
    accumulateOutputEmit(stats, renderMs, elapsedMillis(writeStart, Clock::now()));
    return 0;
}

int
WorkspaceBuilder::emitObjectBundle(CompilationUnit &rootUnit,
                                   const CompileOptions &options,
                                   const std::string &outputPath,
                                   const std::string &cacheOutputPath,
                                   SessionStats &stats,
                                   std::ostream &out) const {
    if (options.ltoMode != CompileOptions::LTOMode::Off) {
        throw DiagnosticError(
            DiagnosticError::Category::Driver,
            "`--emit objects` does not support link-time optimization",
            "Use `--emit obj --lto full` for the explicit slow LTO path.");
    }
    if (outputPath.empty()) {
        throw DiagnosticError(
            DiagnosticError::Category::Driver,
            "multi-object emission requires an explicit manifest output path",
            "Pass an output file when using `--emit objects`.");
    }

    namespace fs = std::filesystem;
    fs::path manifestPath = fs::path(outputPath);
    fs::path bundleStem = manifestPath.filename();
    bundleStem += ".d";
    fs::path bundleDir = cacheOutputPath.empty()
        ? manifestPath.parent_path() / bundleStem
        : fs::path(cacheOutputPath) / bundleStem;
    fs::create_directories(bundleDir);

    int exitCode =
        buildArtifacts(rootUnit, options, true, false, &bundleDir, stats, out);
    if (exitCode != 0) {
        return exitCode;
    }

    out << "format\tlona-object-bundle-v0\n";
    out << "target\t" << normalizeTargetTriple(options.targetTriple) << '\n';

    auto writeStart = Clock::now();
    for (const auto &path : workspace_.moduleGraph().postOrderFrom(rootUnit.path())) {
        auto *artifact = workspace_.findArtifact(path);
        if (artifact == nullptr) {
            throw DiagnosticError(DiagnosticError::Category::Internal,
                                  "bundle emission is missing module artifact `" +
                                      toStdString(path) + "`",
                                  "This looks like a compiler module scheduling bug.");
        }
        if (!artifact->hasObjectCode()) {
            throw DiagnosticError(DiagnosticError::Category::Internal,
                                  "bundle emission is missing module object code for `" +
                                      toStdString(artifact->path()) + "`",
                                  "This looks like a compiler object emission bug.");
        }

        fs::path objectPath = bundleObjectPath(*artifact, bundleDir);
        writeBinaryFile(objectPath, artifact->objectCode());
        out << "object\tmodule\t" << fs::absolute(objectPath).string() << '\n';
    }
    accumulateOutputEmit(stats, 0.0, elapsedMillis(writeStart, Clock::now()));

    return 0;
}

}  // namespace lona
