#include "lona/emit/debug.hh"

#include <llvm-18/llvm/BinaryFormat/Dwarf.h>
#include <llvm-18/llvm/IR/DerivedTypes.h>

namespace lona {
namespace llvmcodegen_impl {

std::string
sourceFilename(const location &loc) {
    return loc.begin.filename ? *loc.begin.filename : std::string();
}

unsigned
sourceLine(const location &loc) {
    return loc.begin.line > 0 ? static_cast<unsigned>(loc.begin.line) : 1U;
}

unsigned
sourceColumn(const location &loc) {
    return loc.begin.column > 0 ? static_cast<unsigned>(loc.begin.column) : 1U;
}

DebugInfoContext::DebugInfoContext(llvm::Module &module, TypeTable &types,
                                   const std::string &sourcePath)
    : module(module), typeTable(types), builder(module) {
    module.addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                         llvm::DEBUG_METADATA_VERSION);
    module.addModuleFlag(llvm::Module::Warning, "Dwarf Version", 5);
    primaryFile = getOrCreateFile(sourcePath.empty() ? module.getName().str()
                                                     : sourcePath);
    compileUnit = builder.createCompileUnit(llvm::dwarf::DW_LANG_C, primaryFile,
                                            "lona", false, "", 0);
}

llvm::DIFile *
DebugInfoContext::getOrCreateFile(const std::string &path) {
    auto key = path.empty() ? std::string("<unknown>") : path;
    auto found = files.find(key);
    if (found != files.end()) {
        return found->second;
    }

    std::string directory = ".";
    std::string filename = key;
    auto slash = key.find_last_of("/\\");
    if (slash != std::string::npos) {
        directory = slash == 0 ? "/" : key.substr(0, slash);
        filename = key.substr(slash + 1);
    }
    if (filename.empty()) {
        filename = module.getName().str();
    }

    auto *file = builder.createFile(filename, directory);
    files.emplace(key, file);
    return file;
}

llvm::DIFile *
DebugInfoContext::fileForLocation(const location &loc) {
    auto path = sourceFilename(loc);
    if (path.empty()) {
        return primaryFile;
    }
    return getOrCreateFile(path);
}

void
DebugInfoContext::finalize() {
    builder.finalize();
}

llvm::DIType *
getOrCreateDebugType(DebugInfoContext &debug, TypeClass *type) {
    if (!type) {
        return nullptr;
    }

    auto found = debug.types.find(type);
    if (found != debug.types.end()) {
        return found->second;
    }

    llvm::DIType *diType = nullptr;
    if (auto *base = asUnqualified<BaseType>(type)) {
        unsigned encoding = llvm::dwarf::DW_ATE_signed;
        switch (base->type) {
            case BaseType::BOOL:
                encoding = llvm::dwarf::DW_ATE_boolean;
                break;
            case BaseType::F32:
            case BaseType::F64:
                encoding = llvm::dwarf::DW_ATE_float;
                break;
            case BaseType::U8:
            case BaseType::U16:
            case BaseType::U32:
            case BaseType::U64:
            case BaseType::USIZE:
                encoding = llvm::dwarf::DW_ATE_unsigned;
                break;
            default:
                break;
        }
        diType = debug.builder.createBasicType(
            toStdString(type->full_name),
            debug.typeTable.getTypeAllocSize(type) * 8, encoding);
    } else if (auto *pointer = asUnqualified<PointerType>(type)) {
        diType = debug.builder.createPointerType(
            getOrCreateDebugType(debug, pointer->getPointeeType()),
            debug.typeTable.getTypeAllocSize(type) * 8, 0, std::nullopt,
            toStdString(type->full_name));
    } else if (auto *indexable = asUnqualified<IndexablePointerType>(type)) {
        diType = debug.builder.createPointerType(
            getOrCreateDebugType(debug, indexable->getElementType()),
            debug.typeTable.getTypeAllocSize(type) * 8, 0, std::nullopt,
            toStdString(type->full_name));
    } else if (auto *array = asUnqualified<ArrayType>(type)) {
        diType = debug.builder.createPointerType(
            getOrCreateDebugType(debug, array->getElementType()),
            debug.typeTable.getTypeAllocSize(type) * 8, 0, std::nullopt,
            toStdString(type->full_name));
    } else if (auto *func = type->as<FuncType>()) {
        std::vector<llvm::Metadata *> elements;
        elements.reserve(func->getArgTypes().size() + 1);
        elements.push_back(getOrCreateDebugType(debug, func->getRetType()));
        for (auto *argType : func->getArgTypes()) {
            elements.push_back(getOrCreateDebugType(debug, argType));
        }
        diType = debug.builder.createSubroutineType(
            debug.builder.getOrCreateTypeArray(elements));
    } else if (asUnqualified<StructType>(type)) {
        diType = debug.builder.createStructType(
            debug.primaryFile, toStdString(type->full_name), debug.primaryFile,
            1, debug.typeTable.getTypeAllocSize(type) * 8, 0,
            llvm::DINode::FlagZero, nullptr,
            debug.builder.getOrCreateArray({}));
    } else {
        diType =
            debug.builder.createUnspecifiedType(toStdString(type->full_name));
    }

    debug.types[type] = diType;
    return diType;
}

llvm::DISubroutineType *
createDebugSubroutineType(DebugInfoContext &debug, FuncType *type) {
    std::vector<llvm::Metadata *> elements;
    if (type) {
        elements.reserve(type->getArgTypes().size() + 1);
        elements.push_back(getOrCreateDebugType(debug, type->getRetType()));
        for (auto *argType : type->getArgTypes()) {
            elements.push_back(getOrCreateDebugType(debug, argType));
        }
    }
    return debug.builder.createSubroutineType(
        debug.builder.getOrCreateTypeArray(elements));
}

llvm::DIScope *
debugScopeFor(DebugInfoContext &debug, llvm::Function *llvmFunc) {
    if (auto *subprogram = llvmFunc->getSubprogram()) {
        return subprogram;
    }
    return debug.primaryFile;
}

llvm::DISubprogram *
createDebugSubprogram(DebugInfoContext &debug, llvm::Function *llvmFunc,
                      FuncType *funcType, llvm::StringRef name,
                      const location &loc) {
    auto *file = debug.fileForLocation(loc);
    auto *subprogram = debug.builder.createFunction(
        file, name, llvmFunc->getName(), file, sourceLine(loc),
        createDebugSubroutineType(debug, funcType), sourceLine(loc),
        llvm::DINode::FlagPrototyped, llvm::DISubprogram::SPFlagDefinition);
    llvmFunc->setSubprogram(subprogram);
    return subprogram;
}

void
applyDebugLocation(llvm::IRBuilder<> &builder, DebugInfoContext *debug,
                   llvm::DIScope *scope, const location &loc) {
    if (!debug || !scope) {
        return;
    }
    builder.SetCurrentDebugLocation(llvm::DILocation::get(
        builder.getContext(), sourceLine(loc), sourceColumn(loc), scope));
}

void
clearDebugLocation(llvm::IRBuilder<> &builder) {
    builder.SetCurrentDebugLocation(llvm::DebugLoc());
}

void
emitDebugDeclare(DebugInfoContext *debug, FuncScope *scope,
                 llvm::DIScope *dbgScope, Object *obj, llvm::StringRef name,
                 TypeClass *type, const location &loc, unsigned argNo) {
    if (!debug || !scope || !dbgScope || !obj || !obj->getllvmValue() ||
        !type) {
        return;
    }

    auto *file = debug->fileForLocation(loc);
    auto *var = argNo == 0 ? debug->builder.createAutoVariable(
                                 dbgScope, name, file, sourceLine(loc),
                                 getOrCreateDebugType(*debug, type))
                           : debug->builder.createParameterVariable(
                                 dbgScope, name, argNo, file, sourceLine(loc),
                                 getOrCreateDebugType(*debug, type), true);

    debug->builder.insertDeclare(
        obj->getllvmValue(), var, debug->builder.createExpression(),
        llvm::DILocation::get(scope->builder.getContext(), sourceLine(loc),
                              sourceColumn(loc), dbgScope),
        scope->builder.GetInsertBlock());
}

}  // namespace llvmcodegen_impl
}  // namespace lona
