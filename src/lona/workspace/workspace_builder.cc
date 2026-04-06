#include "workspace_builder.hh"
#include "lona/abi/abi.hh"
#include "lona/abi/native_abi.hh"
#include "lona/err/err.hh"
#include "lona/resolve/resolve.hh"
#include "lona/sema/hir.hh"
#include "lona/util/time.hh"
#include "lona/visitor.hh"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <llvm-18/llvm/IR/LLVMContext.h>
#include <llvm-18/llvm/IR/Module.h>
#include <llvm-18/llvm/IR/PassManager.h>
#include <llvm-18/llvm/IR/Verifier.h>
#include <llvm-18/llvm/Passes/OptimizationLevel.h>
#include <llvm-18/llvm/Passes/PassBuilder.h>
#include <llvm-18/llvm/Target/TargetMachine.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>
#include <optional>
#include <sstream>
#include <utility>

namespace lona {
namespace workspace_builder_impl {

using Json = nlohmann::json;

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
        passBuilder.buildPerModuleDefaultPipeline(
            getOptimizationLevel(optLevel));
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
parseArtifactBitcodeModule(const ModuleArtifact &artifact,
                           llvm::LLVMContext &context) {
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
        throw DiagnosticError(DiagnosticError::Category::Internal,
                              "failed to link synthetic " + context + " module",
                              "Check for duplicate entry symbols or "
                              "incompatible LLVM module state.");
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
ensureNativeAbiVersionField(llvm::Module &module,
                            llvm::StringRef targetTriple) {
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
    auto *field = new llvm::GlobalVariable(module, init->getType(), true,
                                           llvm::GlobalValue::InternalLinkage,
                                           init, symbolName);
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
    return new llvm::GlobalVariable(
        module, llvm::Type::getInt32Ty(context), false,
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
    return new llvm::GlobalVariable(
        module, ptrTy, false, llvm::GlobalValue::ExternalLinkage,
        llvm::ConstantPointerNull::get(ptrTy), hostedArgvName());
}

std::unique_ptr<llvm::Module>
createHostedMainShimModule(llvm::LLVMContext &context,
                           llvm::StringRef targetTriple) {
    auto module =
        std::make_unique<llvm::Module>("lona.hosted_entry_shim", context);
    configureModuleTargetLayout(*module, targetTriple);

    auto *entryType =
        llvm::FunctionType::get(llvm::Type::getInt32Ty(context), {}, false);
    auto *entryDecl =
        llvm::Function::Create(entryType, llvm::Function::ExternalLinkage,
                               languageEntryName(), *module);
    annotateFunctionAbi(*entryDecl, AbiKind::Native);

    auto *argcGlobal = getOrCreateHostedArgcGlobal(*module);
    auto *argvGlobal = getOrCreateHostedArgvGlobal(*module);

    auto *mainType =
        llvm::FunctionType::get(llvm::Type::getInt32Ty(context),
                                {llvm::Type::getInt32Ty(context),
                                 llvm::PointerType::getUnqual(context)},
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
        throw DiagnosticError(
            DiagnosticError::Category::Internal,
            "LLVM target machine cannot emit object files for the active "
            "target",
            "Check native target initialization and object emission setup.");
    }

    passManager.run(module);
    out.write(objectData.data(),
              static_cast<std::streamsize>(objectData.size()));
    if (!out) {
        throw DiagnosticError(
            DiagnosticError::Category::Driver,
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
            "Check that the path is writable and that parent directories "
            "exist.");
    }

    llvm::legacy::PassManager passManager;
    auto &targetMachine = targetMachineFor(targetTriple);
    if (targetMachine.addPassesToEmitFile(passManager, out, nullptr,
                                          llvm::CodeGenFileType::ObjectFile)) {
        throw DiagnosticError(
            DiagnosticError::Category::Internal,
            "LLVM target machine cannot emit object files for the active "
            "target",
            "Check native target initialization and object emission setup.");
    }

    passManager.run(module);
    out.flush();
    if (out.has_error()) {
        throw DiagnosticError(
            DiagnosticError::Category::Driver,
            "I couldn't write output file `" + outputPath + "`.",
            "Check that the path is writable and that the filesystem has "
            "enough space.");
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
        throw DiagnosticError(
            DiagnosticError::Category::Internal,
            "LLVM target machine cannot emit object files for the active "
            "target",
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
        throw DiagnosticError(
            DiagnosticError::Category::Driver,
            "I couldn't open output file `" + path.string() + "`.",
            "Check that the path is writable and that parent directories "
            "exist.");
    }
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        throw DiagnosticError(
            DiagnosticError::Category::Driver,
            "I couldn't write output file `" + path.string() + "`.",
            "Check that the path is writable and that the filesystem has "
            "enough space.");
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
        throw DiagnosticError(
            DiagnosticError::Category::Driver,
            "I couldn't inspect cached object file `" + path.string() + "`.",
            "Check that the cache directory is readable.");
    }
    in.seekg(0, std::ios::beg);

    ModuleArtifact::ByteBuffer bytes(static_cast<std::size_t>(size));
    if (size > 0) {
        in.read(reinterpret_cast<char *>(bytes.data()), size);
    }
    if (!in) {
        throw DiagnosticError(
            DiagnosticError::Category::Driver,
            "I couldn't read cached object file `" + path.string() + "`.",
            "Check that the cache directory is readable.");
    }
    return bytes;
}

const char *
entryRoleKeyword(ModuleEntryRole entryRole) {
    return entryRole == ModuleEntryRole::Root ? "root" : "dependency";
}

ModuleEntryRole
parseEntryRole(const std::string &text) {
    if (text == "root") {
        return ModuleEntryRole::Root;
    }
    if (text == "dependency") {
        return ModuleEntryRole::Dependency;
    }
    throw DiagnosticError(DiagnosticError::Category::Driver,
                          "cached module metadata has an unknown entry role `" +
                              text + "`",
                          "Clear the object bundle cache and rebuild.");
}

const char *
genericInstanceKindKeyword(GenericInstanceKind kind) {
    switch (kind) {
        case GenericInstanceKind::Function:
            return "function";
        case GenericInstanceKind::Struct:
            return "struct";
        case GenericInstanceKind::Method:
            return "method";
    }
    return "function";
}

GenericInstanceKind
parseGenericInstanceKind(const std::string &text) {
    if (text == "function") {
        return GenericInstanceKind::Function;
    }
    if (text == "struct") {
        return GenericInstanceKind::Struct;
    }
    if (text == "method") {
        return GenericInstanceKind::Method;
    }
    throw DiagnosticError(
        DiagnosticError::Category::Driver,
        "cached generic instance metadata has an unknown kind `" + text + "`",
        "Clear the object bundle cache and rebuild.");
}

Json
encodeGenericInstanceRecord(const GenericInstanceArtifactRecord &record) {
    Json root = Json::object();
    root["requester_module_key"] = toStdString(record.key.requesterModuleKey);
    root["owner_module_key"] = toStdString(record.key.ownerModuleKey);
    root["kind"] = genericInstanceKindKeyword(record.key.kind);
    root["template_name"] = toStdString(record.key.templateName);
    root["method_name"] = toStdString(record.key.methodName);
    root["concrete_type_args"] = Json::array();
    for (const auto &arg : record.key.concreteTypeArgs) {
        root["concrete_type_args"].push_back(toStdString(arg));
    }
    root["owner_interface_hash"] = record.revision.ownerInterfaceHash;
    root["owner_implementation_hash"] = record.revision.ownerImplementationHash;
    root["owner_visible_import_hash"] =
        record.revision.ownerVisibleImportHash;
    root["bound_visible_state_hash"] = record.revision.boundVisibleStateHash;
    root["emitted_symbol_names"] = Json::array();
    for (const auto &symbol : record.emittedSymbolNames) {
        root["emitted_symbol_names"].push_back(toStdString(symbol));
    }
    return root;
}

GenericInstanceArtifactRecord
decodeGenericInstanceRecord(const Json &root) {
    GenericInstanceArtifactRecord record;
    record.key.requesterModuleKey =
        string(root.at("requester_module_key").get<std::string>());
    record.key.ownerModuleKey =
        string(root.at("owner_module_key").get<std::string>());
    record.key.kind = parseGenericInstanceKind(root.at("kind").get<std::string>());
    record.key.templateName =
        string(root.at("template_name").get<std::string>());
    record.key.methodName = string(root.value("method_name", std::string()));
    for (const auto &arg : root.at("concrete_type_args")) {
        record.key.concreteTypeArgs.push_back(string(arg.get<std::string>()));
    }
    record.revision.ownerInterfaceHash =
        root.at("owner_interface_hash").get<std::uint64_t>();
    record.revision.ownerImplementationHash =
        root.at("owner_implementation_hash").get<std::uint64_t>();
    record.revision.ownerVisibleImportHash =
        root.at("owner_visible_import_hash").get<std::uint64_t>();
    record.revision.boundVisibleStateHash =
        root.value("bound_visible_state_hash", static_cast<std::uint64_t>(0));
    for (const auto &symbol : root.at("emitted_symbol_names")) {
        record.emittedSymbolNames.push_back(string(symbol.get<std::string>()));
    }
    return record;
}

Json
encodeArtifactMetadata(const ModuleArtifact &artifact) {
    Json root = Json::object();
    root["format"] = "lona-artifact-bundle-metadata-v1";
    root["path"] = toStdString(artifact.path());
    root["module_key"] = toStdString(artifact.moduleKey());
    root["module_name"] = toStdString(artifact.moduleName());
    root["source_hash"] = artifact.sourceHash();
    root["interface_hash"] = artifact.interfaceHash();
    root["implementation_hash"] = artifact.implementationHash();
    root["target_triple"] = toStdString(artifact.targetTriple());
    root["opt_level"] = artifact.optLevel();
    root["debug_info"] = artifact.debugInfo();
    root["entry_role"] = entryRoleKeyword(artifact.entryRole());
    root["contains_native_abi"] = artifact.containsNativeAbi();
    root["dependency_interface_hashes"] = Json::object();
    for (const auto &[dependencyKey, dependencyHash] :
         artifact.dependencyInterfaceHashes()) {
        root["dependency_interface_hashes"][toStdString(dependencyKey)] =
            dependencyHash;
    }
    root["generic_instance_records"] = Json::array();
    for (const auto &record : artifact.genericInstanceRecords()) {
        root["generic_instance_records"].push_back(
            encodeGenericInstanceRecord(record));
    }
    return root;
}

ModuleArtifact
decodeArtifactMetadata(const Json &root) {
    const auto format = root.value("format", std::string());
    if (format != "lona-object-bundle-metadata-v0" &&
        format != "lona-artifact-bundle-metadata-v1") {
        throw DiagnosticError(
            DiagnosticError::Category::Driver,
            "cached artifact bundle metadata has an unsupported format",
            "Clear the bundle cache and rebuild.");
    }

    ModuleArtifact artifact(
        root.at("path").get<std::string>(), root.at("module_key").get<std::string>(),
        root.at("module_name").get<std::string>(),
        root.at("source_hash").get<std::uint64_t>(),
        root.at("interface_hash").get<std::uint64_t>(),
        root.at("implementation_hash").get<std::uint64_t>());

    std::unordered_map<string, std::uint64_t> dependencyHashes;
    for (auto it = root.at("dependency_interface_hashes").begin();
         it != root.at("dependency_interface_hashes").end(); ++it) {
        dependencyHashes.emplace(string(it.key()), it.value().get<std::uint64_t>());
    }
    artifact.setDependencyInterfaceHashes(std::move(dependencyHashes));
    artifact.setCompileProfile(root.at("target_triple").get<std::string>(),
                               root.at("opt_level").get<int>(),
                               root.at("debug_info").get<bool>(),
                               parseEntryRole(root.at("entry_role").get<std::string>()));
    artifact.setContainsNativeAbi(root.value("contains_native_abi", false));

    std::vector<GenericInstanceArtifactRecord> genericRecords;
    if (auto found = root.find("generic_instance_records");
        found != root.end() && found->is_array()) {
        genericRecords.reserve(found->size());
        for (const auto &item : *found) {
            genericRecords.push_back(decodeGenericInstanceRecord(item));
        }
    }
    artifact.setGenericInstanceRecords(std::move(genericRecords));
    return artifact;
}

std::optional<ModuleArtifact>
readArtifactMetadataIfPresent(const std::filesystem::path &path) {
    std::ifstream in(path);
    if (!in) {
        return std::nullopt;
    }
    Json root;
    try {
        in >> root;
    } catch (const std::exception &err) {
        throw DiagnosticError(
            DiagnosticError::Category::Driver,
            "I couldn't parse cached artifact metadata `" + path.string() + "`.",
            err.what());
    }
    return decodeArtifactMetadata(root);
}

void
writeArtifactMetadata(const std::filesystem::path &path,
                      const ModuleArtifact &artifact) {
    std::ofstream out(path);
    if (!out) {
        throw DiagnosticError(
            DiagnosticError::Category::Driver,
            "I couldn't write cached artifact metadata `" + path.string() + "`.",
            "Check that the cache directory is writable.");
    }
    out << encodeArtifactMetadata(artifact).dump(2) << '\n';
    if (!out) {
        throw DiagnosticError(
            DiagnosticError::Category::Driver,
            "I couldn't finish writing cached artifact metadata `" +
                path.string() + "`.",
            "Check that the cache directory is writable.");
    }
}

const CompilationUnit *
findUnitByModuleKey(const ModuleGraph &moduleGraph, const string &moduleKey) {
    for (const auto &path : moduleGraph.loadOrder()) {
        const auto *unit = moduleGraph.find(path);
        if (unit && unit->moduleKey() == moduleKey) {
            return unit;
        }
    }
    return nullptr;
}

bool
matchesGenericInstanceRecords(const ModuleGraph &moduleGraph,
                              const CompilationUnit &requesterUnit,
                              const ModuleArtifact &artifact) {
    for (const auto &record : artifact.genericInstanceRecords()) {
        if (record.key.requesterModuleKey != requesterUnit.moduleKey()) {
            return false;
        }
        const auto *ownerUnit =
            findUnitByModuleKey(moduleGraph, record.key.ownerModuleKey);
        if (!ownerUnit) {
            return false;
        }
        const GenericTemplateRevision currentRevision{
            ownerUnit->interfaceHash(), ownerUnit->implementationHash(),
            ownerUnit->visibleImportInterfaceHash(),
            requesterUnit.visibleTraitImplHash()};
        if (!(record.revision == currentRevision)) {
            return false;
        }
    }
    return true;
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

using workspace_builder_impl::accumulateArtifactEmit;
using workspace_builder_impl::accumulateOutputEmit;
using workspace_builder_impl::appendHIRFunctions;
using workspace_builder_impl::createHostedMainShimModule;
using workspace_builder_impl::emitBitcodeData;
using workspace_builder_impl::emitObjectData;
using workspace_builder_impl::emitObjectFile;
using workspace_builder_impl::entryRoleKeyword;
using workspace_builder_impl::ensureNativeAbiVersionField;
using workspace_builder_impl::isLanguageEntryType;
using workspace_builder_impl::languageEntryName;
using workspace_builder_impl::linkSyntheticModule;
using workspace_builder_impl::moduleHasFunctionSymbol;
using workspace_builder_impl::moduleUsesNativeAbi;
using workspace_builder_impl::optimizeModule;
using workspace_builder_impl::parseArtifactBitcodeModule;
using workspace_builder_impl::readBinaryFileIfPresent;
using workspace_builder_impl::readArtifactMetadataIfPresent;
using workspace_builder_impl::verifyCompiledModule;
using workspace_builder_impl::writeArtifactMetadata;
using workspace_builder_impl::writeBinaryFile;
using workspace_builder_impl::matchesGenericInstanceRecords;

ModuleEntryRole
WorkspaceBuilder::artifactEntryRoleFor(const CompilationUnit &unit,
                                       const CompilationUnit &rootUnit) {
    return unit.path() == rootUnit.path() ? ModuleEntryRole::Root
                                          : ModuleEntryRole::Dependency;
}

WorkspaceBuilder::WorkspaceBuilder(CompilerWorkspace &workspace,
                                   const WorkspaceLoader &loader)
    : workspace_(workspace),
      loader_(loader),
      executor_(createSerialModuleExecutor()) {
    pipeline_.addStage("collect-declarations", [this](
                                                   IRPipelineContext &context) {
        auto start = Clock::now();
        const bool exportEntryNamespace =
            context.rootUnit &&
            context.rootUnit->path() != context.entryUnit.path();
        auto dependencyStart = Clock::now();
        for (const auto &dependencyPath :
             context.moduleGraph.dependenciesOf(context.entryUnit.path())) {
            auto *loadedUnit = workspace_.moduleGraph().find(dependencyPath);
            if (loadedUnit == nullptr) {
                throw DiagnosticError(
                    DiagnosticError::Category::Internal,
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
        context.stats.entryDeclarationMs +=
            elapsedMillis(entryStart, Clock::now());
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
        auto resolved = resolveModule(
            &context.build.global, context.entryUnit.syntaxTree(),
            &context.entryUnit,
            context.rootUnit != nullptr &&
                context.rootUnit->path() == context.entryUnit.path());
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
                      toStdString(context.entryUnit.path()), &context.entryUnit,
                      &context.moduleGraph);
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
        const bool verifyOk =
            verifyCompiledModule(context.build.module, context.out);
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
WorkspaceBuilder::collectDependencyInterfaceHashes(
    const CompilationUnit &unit) const {
    std::unordered_map<string, std::uint64_t> hashes;
    for (const auto &dependencyPath :
         workspace_.moduleGraph().dependenciesOf(unit.path())) {
        auto *dependency = workspace_.moduleGraph().find(dependencyPath);
        if (dependency == nullptr) {
            throw DiagnosticError(
                DiagnosticError::Category::Internal,
                "module graph dependency references a missing unit",
                "This looks like a compiler module graph bug.");
        }
        hashes.emplace(dependency->moduleKey(), dependency->interfaceHash());
    }
    return hashes;
}

std::string
WorkspaceBuilder::bundleMemberFileName(const ModuleArtifact &artifact,
                                       BundleArtifactKind kind) const {
    std::string stem = artifact.moduleName().empty()
                           ? "module"
                           : toStdString(artifact.moduleName());
    for (char &ch : stem) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (!(std::isalnum(byte) || ch == '_' || ch == '-')) {
            ch = '_';
        }
    }

    std::vector<std::pair<string, std::uint64_t>> dependencies(
        artifact.dependencyInterfaceHashes().begin(),
        artifact.dependencyInterfaceHashes().end());
    std::sort(
        dependencies.begin(), dependencies.end(),
        [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });

    std::ostringstream cacheKey;
    cacheKey << artifact.moduleKey() << "|" << artifact.targetTriple() << "|"
             << (kind == BundleArtifactKind::Object ? "O" : "B")
             << artifact.optLevel() << "|"
             << (artifact.debugInfo() ? "g" : "ng")
             << "|src=" << artifact.sourceHash()
             << "|iface=" << artifact.interfaceHash()
             << "|impl=" << artifact.implementationHash();
    for (const auto &[dependencyKey, dependencyHash] : dependencies) {
        cacheKey << "|dep=" << dependencyKey << ":" << dependencyHash;
    }

    std::ostringstream suffix;
    suffix << std::hex << std::setw(16) << std::setfill('0')
           << static_cast<unsigned long long>(
                  std::hash<std::string>{}(cacheKey.str()));
    const char *roleSuffix =
        artifact.entryRole() == ModuleEntryRole::Root ? "root" : "dependency";
    const char *extension =
        kind == BundleArtifactKind::Object ? ".o" : ".bc";
    return stem + "-" + roleSuffix + "-" + suffix.str() + extension;
}

std::filesystem::path
WorkspaceBuilder::bundleMemberPath(
    const ModuleArtifact &artifact,
    const std::filesystem::path &bundleDir, BundleArtifactKind kind) const {
    return bundleDir / bundleMemberFileName(artifact, kind);
}

void
WorkspaceBuilder::persistArtifactOutput(
    const ModuleArtifact &artifact,
    const std::filesystem::path &artifactCacheDir,
    BundleArtifactKind kind) const {
    std::filesystem::create_directories(artifactCacheDir);
    const auto memberPath = bundleMemberPath(artifact, artifactCacheDir, kind);
    if (kind == BundleArtifactKind::Object) {
        if (!artifact.hasObjectCode()) {
            throw DiagnosticError(
                DiagnosticError::Category::Internal,
                "artifact cache write is missing object code for `" +
                    toStdString(artifact.path()) + "`",
                "This looks like a compiler object caching bug.");
        }
        writeBinaryFile(memberPath, artifact.objectCode());
    } else {
        if (!artifact.hasBitcode()) {
            throw DiagnosticError(
                DiagnosticError::Category::Internal,
                "artifact cache write is missing bitcode for `" +
                    toStdString(artifact.path()) + "`",
                "This looks like a compiler bitcode caching bug.");
        }
        writeBinaryFile(memberPath, artifact.bitcode());
    }
    writeArtifactMetadata(std::filesystem::path(memberPath.string() + ".meta.json"),
                          artifact);
}

bool
WorkspaceBuilder::matchesArtifact(const CompilationUnit &unit,
                                  const ModuleArtifact &artifact,
                                  const CompileOptions &options,
                                  ModuleEntryRole entryRole) const {
    if (artifact.sourceHash() != unit.sourceHash() ||
        artifact.interfaceHash() != unit.interfaceHash() ||
        artifact.implementationHash() != unit.implementationHash()) {
        return false;
    }
    if (toStdString(artifact.targetTriple()) !=
            normalizeTargetTriple(options.targetTriple) ||
        artifact.optLevel() != options.optLevel ||
        artifact.debugInfo() != options.debugInfo ||
        artifact.entryRole() != entryRole) {
        return false;
    }
    if (artifact.dependencyInterfaceHashes() !=
        collectDependencyInterfaceHashes(unit)) {
        return false;
    }
    return matchesGenericInstanceRecords(workspace_.moduleGraph(), unit,
                                         artifact);
}

ModuleArtifact *
WorkspaceBuilder::reusableArtifactFor(const CompilationUnit &unit,
                                      const CompileOptions &options,
                                      const CompilationUnit &rootUnit) const {
    if (options.noCache) {
        return nullptr;
    }
    auto *artifact = workspace_.findArtifact(
        unit.path(), artifactEntryRoleFor(unit, rootUnit));
    if (artifact == nullptr) {
        return nullptr;
    }
    return matchesArtifact(unit, *artifact, options,
                           artifactEntryRoleFor(unit, rootUnit))
               ? artifact
               : nullptr;
}

ModuleArtifact
WorkspaceBuilder::createArtifact(const CompilationUnit &unit,
                                 const CompileOptions &options,
                                 const CompilationUnit &rootUnit) const {
    const auto entryRole = artifactEntryRoleFor(unit, rootUnit);
    ModuleArtifact artifact(unit.path(), unit.moduleKey(), unit.moduleName(),
                            unit.sourceHash(), unit.interfaceHash(),
                            unit.implementationHash());
    artifact.setDependencyInterfaceHashes(
        collectDependencyInterfaceHashes(unit));
    artifact.setCompileProfile(normalizeTargetTriple(options.targetTriple),
                               options.optLevel, options.debugInfo, entryRole);
    return artifact;
}

int
WorkspaceBuilder::ensureArtifactOutputs(ModuleArtifact &artifact,
                                        const CompileOptions &options,
                                        bool requireObjects,
                                        bool requireBitcode,
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
                                 const std::filesystem::path *artifactCacheDir,
                                 SessionStats &stats, std::ostream &out) const {
    workspace_.buildQueue().reset(workspace_.moduleGraph(), rootUnit.path());
    return executor_->execute(
        workspace_.buildQueue(), [&](const string &path) -> int {
            auto *queuedUnit = workspace_.moduleGraph().find(path);
            if (queuedUnit == nullptr) {
                throw DiagnosticError(
                    DiagnosticError::Category::Internal,
                    "module build queue references a missing unit",
                    "This looks like a compiler module queue bug.");
            }

            auto cacheLookupStart = Clock::now();
            auto *cachedArtifact =
                reusableArtifactFor(*queuedUnit, options, rootUnit);
            stats.cacheLookupMs +=
                elapsedMillis(cacheLookupStart, Clock::now());
            if (cachedArtifact != nullptr && requireBitcode &&
                !cachedArtifact->hasBitcode()) {
                cachedArtifact = nullptr;
            }
            if (cachedArtifact != nullptr && requireObjects &&
                !cachedArtifact->hasObjectCode() &&
                !cachedArtifact->hasBitcode()) {
                cachedArtifact = nullptr;
            }
            if (cachedArtifact != nullptr) {
                queuedUnit->markCompiled();
                ++stats.reusedModules;
                int artifactExitCode = ensureArtifactOutputs(
                    *cachedArtifact, options, requireObjects, requireBitcode,
                    stats);
                if (artifactExitCode != 0) {
                    return artifactExitCode;
                }
                if (artifactCacheDir != nullptr &&
                    (requireObjects != requireBitcode)) {
                    persistArtifactOutput(
                        *cachedArtifact, *artifactCacheDir,
                        requireObjects ? BundleArtifactKind::Object
                                       : BundleArtifactKind::Bitcode);
                }
                return 0;
            }

            ModuleArtifact artifact =
                createArtifact(*queuedUnit, options, rootUnit);
            if (!options.noCache && artifactCacheDir != nullptr &&
                (requireObjects != requireBitcode)) {
                const auto bundleKind = requireObjects
                                            ? BundleArtifactKind::Object
                                            : BundleArtifactKind::Bitcode;
                auto memberPath =
                    bundleMemberPath(artifact, *artifactCacheDir, bundleKind);
                auto metadataPath = std::filesystem::path(
                    memberPath.string() +
                    ".meta.json");
                auto cacheRestoreStart = Clock::now();
                auto cachedMetadata =
                    readArtifactMetadataIfPresent(metadataPath);
                stats.cacheRestoreMs +=
                    elapsedMillis(cacheRestoreStart, Clock::now());
                if (cachedMetadata.has_value() &&
                    matchesArtifact(*queuedUnit, *cachedMetadata, options,
                                    artifact.entryRole())) {
                    auto cachedBytes = readBinaryFileIfPresent(memberPath);
                    if (cachedBytes.has_value()) {
                        if (bundleKind == BundleArtifactKind::Object) {
                            cachedMetadata->setObjectCode(
                                std::move(*cachedBytes));
                        } else {
                            cachedMetadata->setBitcode(std::move(*cachedBytes));
                        }
                        workspace_.storeArtifact(std::move(*cachedMetadata));
                        queuedUnit->markCompiled();
                        ++stats.reusedModules;
                        if (bundleKind == BundleArtifactKind::Object) {
                            ++stats.reusedModuleObjects;
                        } else {
                            ++stats.reusedModuleBitcode;
                        }
                        return 0;
                    }
                }
            }
            int moduleExitCode =
                compileModule(*queuedUnit, options, artifact, requireObjects,
                              requireBitcode, stats, out);
            if (moduleExitCode != 0) {
                return moduleExitCode;
            }
            if (artifactCacheDir != nullptr && (requireObjects != requireBitcode)) {
                persistArtifactOutput(
                    artifact, *artifactCacheDir,
                    requireObjects ? BundleArtifactKind::Object
                                   : BundleArtifactKind::Bitcode);
            }
            workspace_.storeArtifact(std::move(artifact));
            return 0;
        });
}

int
WorkspaceBuilder::compileModule(CompilationUnit &unit,
                                const CompileOptions &options,
                                ModuleArtifact &artifact, bool emitObject,
                                bool emitBitcode, SessionStats &stats,
                                std::ostream &out) const {
    unit.clearResolvedTypes();
    std::ostringstream ir;
    IRPipelineContext context(unit, workspace_.moduleGraph(), options, ir,
                              stats);
    context.rootUnit = workspace_.moduleGraph().root();
    context.captureIRText = false;
    int exitCode = pipeline_.run(context);
    if (exitCode == 0) {
        unit.markCompiled();
        artifact.setContainsNativeAbi(
            moduleUsesNativeAbi(context.build.module));
        artifact.setGenericInstanceRecords(unit.recordedGenericInstances());
        if (emitBitcode) {
            auto emitStart = Clock::now();
            artifact.setBitcode(emitBitcodeData(context.build.module));
            accumulateArtifactEmit(stats,
                                   elapsedMillis(emitStart, Clock::now()));
            ++stats.emittedModuleBitcode;
        }
        if (emitObject) {
            ensureNativeAbiVersionField(context.build.module,
                                        options.targetTriple);
            auto emitStart = Clock::now();
            artifact.setObjectCode(
                emitObjectData(context.build.module, options.targetTriple));
            accumulateArtifactEmit(stats,
                                   elapsedMillis(emitStart, Clock::now()));
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
                                SessionStats &stats, std::ostream &out) const {
    auto *rootArtifact =
        workspace_.findArtifact(rootUnit.path(), ModuleEntryRole::Root);
    if (rootArtifact == nullptr) {
        throw DiagnosticError(
            DiagnosticError::Category::Internal,
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
    for (const auto &path :
         workspace_.moduleGraph().postOrderFrom(rootUnit.path())) {
        if (path == rootUnit.path()) {
            continue;
        }
        auto *artifact =
            workspace_.findArtifact(path, ModuleEntryRole::Dependency);
        if (artifact == nullptr) {
            throw DiagnosticError(
                DiagnosticError::Category::Internal,
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
                "Check for duplicate IR symbols or incompatible LLVM module "
                "state.");
        }
        linkMergeMs += elapsedMillis(mergeStart, Clock::now());
    }

    const bool hasLanguageEntry =
        moduleHasFunctionSymbol(*linkedModule, languageEntryName());
    if (hasLanguageEntry && hostedEntry &&
        !moduleHasFunctionSymbol(*linkedModule, "main")) {
        auto mergeStart = Clock::now();
        linkSyntheticModule(linker,
                            createHostedMainShimModule(
                                *context, linkedModule->getTargetTriple()),
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
    return root ? workspace_.moduleGraph().postOrderFrom(root->path()).size()
                : 0;
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
    auto hostedShim = createHostedMainShimModule(
        *context, normalizeTargetTriple(options.targetTriple));
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
WorkspaceBuilder::emitIR(CompilationUnit &rootUnit,
                         const CompileOptions &options, SessionStats &stats,
                         std::ostream &out) const {
    const bool useLTO = options.ltoMode == CompileOptions::LTOMode::Full;
    int exitCode =
        buildArtifacts(rootUnit, options, false, true, nullptr, stats, out);
    if (exitCode != 0) {
        return exitCode;
    }

    auto linked =
        linkArtifacts(rootUnit, targetUsesHostedEntry(options.targetTriple),
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
WorkspaceBuilder::emitLinkedObject(CompilationUnit &rootUnit,
                                   const CompileOptions &options,
                                   const std::string &outputPath,
                                   const std::string &artifactCachePath,
                                   SessionStats &stats,
                                   std::ostream &out) const {
    const bool useLTO = options.ltoMode == CompileOptions::LTOMode::Full;
    std::optional<std::filesystem::path> artifactCacheDir;
    if (!artifactCachePath.empty()) {
        artifactCacheDir = std::filesystem::path(artifactCachePath);
    } else if (!outputPath.empty()) {
        artifactCacheDir = std::filesystem::path(outputPath + ".d");
    }
    int exitCode =
        buildArtifacts(rootUnit, options, false, true,
                       artifactCacheDir ? &*artifactCacheDir : nullptr, stats,
                       out);
    if (exitCode != 0) {
        return exitCode;
    }

    auto linked =
        linkArtifacts(rootUnit, targetUsesHostedEntry(options.targetTriple),
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
        accumulateOutputEmit(stats, elapsedMillis(emitStart, Clock::now()),
                             0.0);
        return 0;
    }

    auto renderStart = Clock::now();
    auto bytes = emitObjectData(*linked.module, options.targetTriple);
    auto renderMs = elapsedMillis(renderStart, Clock::now());
    auto writeStart = Clock::now();
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        throw DiagnosticError(
            DiagnosticError::Category::Driver,
            "I couldn't write the emitted object file.",
            "Check that the destination stream or file is writable.");
    }
    accumulateOutputEmit(stats, renderMs,
                         elapsedMillis(writeStart, Clock::now()));
    return 0;
}

int
WorkspaceBuilder::emitBitcodeBundle(CompilationUnit &rootUnit,
                                    const CompileOptions &options,
                                    const std::string &outputPath,
                                    const std::string &cacheOutputPath,
                                    SessionStats &stats,
                                    std::ostream &out) const {
    if (options.ltoMode != CompileOptions::LTOMode::Off) {
        throw DiagnosticError(
            DiagnosticError::Category::Driver,
            "`--emit bc` does not support link-time optimization",
            "Use `--emit linked-obj --lto full` for the explicit slow LTO "
            "path.");
    }
    if (outputPath.empty()) {
        throw DiagnosticError(
            DiagnosticError::Category::Driver,
            "bitcode bundle emission requires an explicit manifest output path",
            "Pass an output file when using `--emit bc`.");
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
        buildArtifacts(rootUnit, options, false, true, &bundleDir, stats, out);
    if (exitCode != 0) {
        return exitCode;
    }

    out << "format\tlona-artifact-bundle-v1\n";
    out << "kind\tbc\n";
    out << "target\t" << normalizeTargetTriple(options.targetTriple) << '\n';

    auto writeStart = Clock::now();
    for (const auto &path :
         workspace_.moduleGraph().postOrderFrom(rootUnit.path())) {
        auto *unit = workspace_.moduleGraph().find(path);
        if (unit == nullptr) {
            throw DiagnosticError(
                DiagnosticError::Category::Internal,
                "bundle emission references a missing module `" +
                    toStdString(path) + "`",
                "This looks like a compiler module graph bug.");
        }
        auto *artifact = workspace_.findArtifact(
            path, artifactEntryRoleFor(*unit, rootUnit));
        if (artifact == nullptr) {
            throw DiagnosticError(
                DiagnosticError::Category::Internal,
                "bundle emission is missing module artifact `" +
                    toStdString(path) + "`",
                "This looks like a compiler module scheduling bug.");
        }
        if (!artifact->hasBitcode()) {
            throw DiagnosticError(
                DiagnosticError::Category::Internal,
                "bundle emission is missing module bitcode for `" +
                    toStdString(artifact->path()) + "`",
                "This looks like a compiler bitcode emission bug.");
        }

        fs::path bitcodePath =
            bundleMemberPath(*artifact, bundleDir, BundleArtifactKind::Bitcode);
        out << "artifact\tbc\t" << entryRoleKeyword(artifact->entryRole())
            << '\t' << fs::absolute(bitcodePath).string() << '\n';
    }
    accumulateOutputEmit(stats, 0.0, elapsedMillis(writeStart, Clock::now()));

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
            "`--emit obj` does not support link-time optimization",
            "Use `--emit linked-obj --lto full` for the explicit slow LTO "
            "path.");
    }
    if (outputPath.empty()) {
        throw DiagnosticError(
            DiagnosticError::Category::Driver,
            "object bundle emission requires an explicit manifest output path",
            "Pass an output file when using `--emit obj`.");
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

    out << "format\tlona-artifact-bundle-v1\n";
    out << "kind\tobj\n";
    out << "target\t" << normalizeTargetTriple(options.targetTriple) << '\n';

    auto writeStart = Clock::now();
    for (const auto &path :
         workspace_.moduleGraph().postOrderFrom(rootUnit.path())) {
        auto *unit = workspace_.moduleGraph().find(path);
        if (unit == nullptr) {
            throw DiagnosticError(
                DiagnosticError::Category::Internal,
                "bundle emission references a missing module `" +
                    toStdString(path) + "`",
                "This looks like a compiler module graph bug.");
        }
        auto *artifact = workspace_.findArtifact(
            path, artifactEntryRoleFor(*unit, rootUnit));
        if (artifact == nullptr) {
            throw DiagnosticError(
                DiagnosticError::Category::Internal,
                "bundle emission is missing module artifact `" +
                    toStdString(path) + "`",
                "This looks like a compiler module scheduling bug.");
        }
        if (!artifact->hasObjectCode()) {
            throw DiagnosticError(
                DiagnosticError::Category::Internal,
                "bundle emission is missing module object code for `" +
                    toStdString(artifact->path()) + "`",
                "This looks like a compiler object emission bug.");
        }

        fs::path objectPath =
            bundleMemberPath(*artifact, bundleDir, BundleArtifactKind::Object);
        out << "artifact\tobj\t" << entryRoleKeyword(artifact->entryRole())
            << '\t' << fs::absolute(objectPath).string() << '\n';
    }
    accumulateOutputEmit(stats, 0.0, elapsedMillis(writeStart, Clock::now()));

    return 0;
}

}  // namespace lona
