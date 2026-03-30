#pragma once

#include "lona/module/compilation_unit.hh"
#include "lona/util/string.hh"
#include <llvm/ADT/StringRef.h>
#include <string>

namespace lona {

inline llvm::StringRef
languageEntrySymbolName() {
    return "__lona_main__";
}

inline std::string
mangleModuleEntryComponent(llvm::StringRef text) {
    static constexpr char kHex[] = "0123456789abcdef";

    std::string mangled;
    mangled.reserve(text.size() * 3);
    for (unsigned char ch : text.bytes()) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9')) {
            mangled.push_back(static_cast<char>(ch));
            continue;
        }
        mangled.push_back('_');
        mangled.push_back(kHex[ch >> 4]);
        mangled.push_back(kHex[ch & 0x0f]);
    }
    if (mangled.empty()) {
        mangled = "module";
    }
    return mangled;
}

inline std::string
mangleModuleEntryComponent(const ::string &text) {
    return mangleModuleEntryComponent(toStringRef(text));
}

inline std::string
moduleInitEntrySymbolName(const CompilationUnit &unit) {
    return "__" + mangleModuleEntryComponent(unit.moduleKey()) +
           "_init_entry__";
}

inline std::string
moduleInitStateSymbolName(const CompilationUnit &unit) {
    return "__" + mangleModuleEntryComponent(unit.moduleKey()) +
           "_init_state__";
}

inline std::string
moduleInitResultSymbolName(const CompilationUnit &unit) {
    return "__" + mangleModuleEntryComponent(unit.moduleKey()) +
           "_init_result__";
}

}  // namespace lona
