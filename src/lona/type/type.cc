#include "type.hh"
#include "../abi/native_abi.hh"
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <mutex>
#include <stdexcept>

namespace lona {

namespace {

struct NativeTargetLayout {
    std::string triple;
    std::unique_ptr<llvm::TargetMachine> machine;
    llvm::DataLayout dataLayout;

    NativeTargetLayout()
        : triple(llvm::sys::getDefaultTargetTriple()),
          dataLayout("") {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();

        std::string error;
        auto *target = llvm::TargetRegistry::lookupTarget(triple, error);
        if (!target) {
            throw std::runtime_error("failed to resolve native LLVM target: " +
                                     error);
        }

        llvm::TargetOptions options;
        machine.reset(target->createTargetMachine(triple, "generic", "",
                                                  options, std::nullopt));
        if (!machine) {
            throw std::runtime_error(
                "failed to create native LLVM target machine");
        }
        dataLayout = machine->createDataLayout();
    }
};

NativeTargetLayout &
nativeTargetLayout() {
    static std::once_flag once;
    static std::unique_ptr<NativeTargetLayout> layout;
    static std::string initError;
    std::call_once(once, [] {
        try {
            layout = std::make_unique<NativeTargetLayout>();
        } catch (const std::exception &ex) {
            initError = ex.what();
        }
    });
    if (!layout) {
        throw std::runtime_error(initError.empty()
                                     ? "failed to initialize LLVM target layout"
                                     : initError);
    }
    return *layout;
}

}  // namespace

const llvm::DataLayout &
defaultTargetDataLayout() {
    return nativeTargetLayout().dataLayout;
}

const std::string &
defaultTargetTriple() {
    return nativeTargetLayout().triple;
}

void
configureModuleTargetLayout(llvm::Module &module) {
    module.setTargetTriple(defaultTargetTriple());
    module.setDataLayout(defaultTargetDataLayout());
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
    return getNativeAbiFunctionType(types, this, false);
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
