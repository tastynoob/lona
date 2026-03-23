#include "type.hh"
#include "../abi/abi.hh"
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace lona {

namespace {

struct TargetLayout {
    std::string triple;
    llvm::Reloc::Model relocModel;
    std::unique_ptr<llvm::TargetMachine> machine;
    llvm::DataLayout dataLayout;

    explicit TargetLayout(const std::string &requestedTriple)
        : triple(normalizeTargetTriple(requestedTriple)),
          relocModel(targetUsesHostedEntry(triple) ? llvm::Reloc::PIC_
                                                   : llvm::Reloc::Static),
          dataLayout("") {
        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmPrinters();
        llvm::InitializeAllAsmParsers();

        std::string error;
        auto *target = llvm::TargetRegistry::lookupTarget(triple, error);
        if (!target) {
            throw std::runtime_error("failed to resolve LLVM target `" + triple +
                                     "`: " + error);
        }

        llvm::TargetOptions options;
        machine.reset(target->createTargetMachine(triple, "generic", "",
                                                  options, relocModel));
        if (!machine) {
            throw std::runtime_error("failed to create LLVM target machine for `" +
                                     triple + "`");
        }
        dataLayout = machine->createDataLayout();
    }
};

std::string
targetLayoutCacheKey(llvm::StringRef triple) {
    return normalizeTargetTriple(triple.str());
}

TargetLayout &
targetLayoutFor(llvm::StringRef triple) {
    static std::once_flag once;
    static std::unordered_map<std::string, std::unique_ptr<TargetLayout>> layouts;
    static std::string initError;
    static std::mutex mutex;
    std::call_once(once, [] {
        try {
            llvm::InitializeAllTargets();
            llvm::InitializeAllTargetMCs();
            llvm::InitializeAllAsmPrinters();
            llvm::InitializeAllAsmParsers();
        } catch (const std::exception &ex) {
            initError = ex.what();
        }
    });
    if (!initError.empty()) {
        throw std::runtime_error(initError.empty()
                                     ? "failed to initialize LLVM target layout"
                                     : initError);
    }

    const std::string key = targetLayoutCacheKey(triple);
    std::lock_guard<std::mutex> lock(mutex);
    auto it = layouts.find(key);
    if (it == layouts.end()) {
        auto layout = std::make_unique<TargetLayout>(key);
        it = layouts.emplace(key, std::move(layout)).first;
    }
    return *it->second;
}

}  // namespace

std::string
normalizeTargetTriple(const std::string &triple) {
    const std::string requested =
        triple.empty() ? llvm::sys::getDefaultTargetTriple() : triple;
    return llvm::Triple::normalize(requested);
}

bool
targetUsesHostedEntry(llvm::StringRef triple) {
    llvm::Triple parsed(normalizeTargetTriple(triple.str()));
    return parsed.getOS() != llvm::Triple::UnknownOS;
}

const llvm::DataLayout &
defaultTargetDataLayout() {
    return targetLayoutFor(defaultTargetTriple()).dataLayout;
}

const std::string &
defaultTargetTriple() {
    static const std::string triple = normalizeTargetTriple("");
    return triple;
}

llvm::TargetMachine &
targetMachineFor(llvm::StringRef triple) {
    return *targetLayoutFor(triple).machine;
}

void
configureModuleTargetLayout(llvm::Module &module, llvm::StringRef triple) {
    auto &layout = targetLayoutFor(triple);
    module.setTargetTriple(layout.triple);
    module.setDataLayout(layout.dataLayout);
}

TypeClass *
stripTopLevelConst(TypeClass *type) {
    auto *qualified = type ? type->as<ConstType>() : nullptr;
    return qualified ? qualified->getBaseType() : type;
}

bool
isConstQualifiedType(TypeClass *type) {
    return type && type->as<ConstType>() != nullptr;
}

namespace {

TypeClass *
rematerializeArrayType(TypeTable *typeTable, ArrayType *array,
                       TypeClass *elementType) {
    if (!array || !elementType) {
        return nullptr;
    }
    if (elementType == array->getElementType()) {
        return array;
    }
    if (typeTable) {
        return typeTable->createArrayType(elementType, array->getDimensions());
    }
    return new ArrayType(elementType, array->getDimensions());
}

TypeClass *
rematerializeTupleType(TypeTable *typeTable, TupleType *tuple,
                       const std::vector<TypeClass *> &itemTypes,
                       bool reusedOriginalItems) {
    if (!tuple) {
        return nullptr;
    }
    if (reusedOriginalItems) {
        return tuple;
    }
    if (typeTable) {
        return typeTable->getOrCreateTupleType(itemTypes);
    }
    return new TupleType(std::vector<TypeClass *>(itemTypes));
}

}  // namespace

TypeClass *
materializeValueType(TypeTable *typeTable, TypeClass *type) {
    if (!type) {
        return nullptr;
    }
    if (auto *qualified = type->as<ConstType>()) {
        return materializeValueType(typeTable, qualified->getBaseType());
    }
    if (auto *array = type->as<ArrayType>()) {
        auto *elementType = materializeValueType(typeTable, array->getElementType());
        return rematerializeArrayType(typeTable, array, elementType);
    }
    if (auto *tuple = type->as<TupleType>()) {
        std::vector<TypeClass *> itemTypes;
        itemTypes.reserve(tuple->getItemTypes().size());
        bool reusedOriginalItems = true;
        for (auto *itemType : tuple->getItemTypes()) {
            auto *materializedItem = materializeValueType(typeTable, itemType);
            reusedOriginalItems = reusedOriginalItems && materializedItem == itemType;
            itemTypes.push_back(materializedItem);
        }
        return rematerializeTupleType(typeTable, tuple, itemTypes,
                                      reusedOriginalItems);
    }
    return type;
}

bool
isConstQualificationConvertible(TypeClass *targetType, TypeClass *sourceType) {
    if (!targetType || !sourceType) {
        return false;
    }
    if (targetType == sourceType) {
        return true;
    }

    auto *targetConst = targetType->as<ConstType>();
    auto *sourceConst = sourceType->as<ConstType>();
    if (targetConst) {
        return isConstQualificationConvertible(
            targetConst->getBaseType(),
            sourceConst ? sourceConst->getBaseType() : sourceType);
    }
    if (sourceConst) {
        return false;
    }

    if (auto *targetPointer = targetType->as<PointerType>()) {
        auto *sourcePointer = sourceType->as<PointerType>();
        return sourcePointer &&
            isConstQualificationConvertible(targetPointer->getPointeeType(),
                                            sourcePointer->getPointeeType());
    }
    if (auto *targetIndexable = targetType->as<IndexablePointerType>()) {
        auto *sourceIndexable = sourceType->as<IndexablePointerType>();
        return sourceIndexable &&
            isConstQualificationConvertible(targetIndexable->getElementType(),
                                            sourceIndexable->getElementType());
    }
    if (auto *targetArray = targetType->as<ArrayType>()) {
        auto *sourceArray = sourceType->as<ArrayType>();
        return sourceArray &&
            targetArray->getDimensions() == sourceArray->getDimensions() &&
            isConstQualificationConvertible(targetArray->getElementType(),
                                            sourceArray->getElementType());
    }
    if (auto *targetTuple = targetType->as<TupleType>()) {
        auto *sourceTuple = sourceType->as<TupleType>();
        if (!sourceTuple ||
            targetTuple->getItemTypes().size() !=
                sourceTuple->getItemTypes().size()) {
            return false;
        }
        for (std::size_t i = 0; i < targetTuple->getItemTypes().size(); ++i) {
            if (!isConstQualificationConvertible(targetTuple->getItemTypes()[i],
                                                 sourceTuple->getItemTypes()[i])) {
                return false;
            }
        }
        return true;
    }
    return targetType == sourceType;
}

bool
isFullyWritableStructFieldType(TypeClass *type) {
    if (!type) {
        return false;
    }
    if (isConstQualifiedType(type)) {
        return false;
    }
    if (type->as<BaseType>() || type->as<StructType>() || type->as<FuncType>() ||
        type->as<PointerType>() || type->as<IndexablePointerType>()) {
        return true;
    }
    if (auto *array = type->as<ArrayType>()) {
        return isFullyWritableStructFieldType(array->getElementType());
    }
    if (auto *tuple = type->as<TupleType>()) {
        for (auto *itemType : tuple->getItemTypes()) {
            if (!isFullyWritableStructFieldType(itemType)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

bool
TupleType::getMember(llvm::StringRef name, ValueTy &member) const {
    if (!name.consume_front("_") || name.empty()) {
        return false;
    }

    unsigned index = 0;
    if (name.getAsInteger(10, index) || index == 0 ||
        index > itemTypes.size()) {
        return false;
    }

    member = {itemTypes[index - 1], static_cast<int>(index - 1)};
    return true;
}

llvm::Type *
BaseType::buildLLVMType(TypeTable& types) {
    switch (type) {
        case Type::U8:
        case Type::I8:
            return llvm::Type::getInt8Ty(types.getContext());
        case Type::U16:
        case Type::I16:
            return llvm::Type::getInt16Ty(types.getContext());
        case Type::U32:
        case Type::I32:
            return llvm::Type::getInt32Ty(types.getContext());
        case Type::U64:
        case Type::I64:
            return llvm::Type::getInt64Ty(types.getContext());
        case Type::F32:
            return llvm::Type::getFloatTy(types.getContext());
        case Type::F64:
            return llvm::Type::getDoubleTy(types.getContext());
        case Type::BOOL:
            return llvm::Type::getInt8Ty(types.getContext());
        default:
            throw "Unsupported base type";
    }
}

llvm::Type *
ConstType::buildLLVMType(TypeTable &types) {
    return baseType ? types.getLLVMType(baseType) : nullptr;
}

llvm::Type *
StructType::buildLLVMType(TypeTable &types) {
    return llvm::StructType::create(types.getContext(),
                                    full_name.tochara());
}

llvm::Type *
TupleType::buildLLVMType(TypeTable &types) {
    std::vector<llvm::Type *> memberTypes;
    memberTypes.reserve(itemTypes.size());
    for (auto *itemType : itemTypes) {
        memberTypes.push_back(types.getLLVMType(itemType));
    }
    return llvm::StructType::get(types.getContext(), memberTypes, false);
}

ObjectPtr
TupleType::newObj(uint32_t specifiers) {
    return new TupleVar(this, specifiers);
}

llvm::Type *
FuncType::buildLLVMType(TypeTable &types) {
    return getFunctionAbiLLVMType(types, this, false);
}

llvm::Type *
PointerType::buildLLVMType(TypeTable &types) {
    return llvm::PointerType::getUnqual(types.getLLVMType(pointeeType));
}

llvm::Type *
IndexablePointerType::buildLLVMType(TypeTable &types) {
    return llvm::PointerType::getUnqual(types.getLLVMType(elementType));
}

llvm::Type *
ArrayType::buildLLVMType(TypeTable &types) {
    bool ok = false;
    auto extents = staticDimensions(&ok);
    if (!ok || extents.empty()) {
        throw std::runtime_error(
            "array LLVM lowering requires fixed explicit dimensions");
    }

    llvm::Type *llvmType = types.getLLVMType(elementType);
    for (auto it = extents.rbegin(); it != extents.rend(); ++it) {
        llvmType = llvm::ArrayType::get(llvmType, static_cast<std::uint64_t>(*it));
    }
    return llvmType;
}

}  // namespace lona
