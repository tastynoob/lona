#pragma once

#include "type/typeclass.hh"


namespace lona {

class I32Type : public BaseType {
public:
    I32Type(llvm::IRBuilder<> &builder) : BaseType(builder.getInt32Ty(), I32) {}
    BaseVariable* binaryOperation(llvm::IRBuilder<> &builder, BaseVariable* left, token_type op, BaseVariable* right) override;
};


}