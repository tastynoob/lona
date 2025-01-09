#pragma once

#include "ast/astnode.hh"
#include "llvm.hh"
#include "type/typeclass.hh"

namespace lona {

// element base selector
class BaseSelector {
public:
    virtual BaseVariable *get(BaseVariable *value) = 0;
};

class ArraySelector : public BaseSelector {
    BaseVariable *array_idx;

public:
    BaseVariable *get(BaseVariable *value) override;
};

class MemberSelector : public BaseSelector {
    uint32_t member_idx;

public:
    BaseVariable *get(BaseVariable *value) override;
};

}  // namespace lona