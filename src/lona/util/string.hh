#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <llvm-18/llvm/ADT/StringRef.h>
#include <sys/types.h>
#include <charconv>

#include "panic.hh"

// COW string implementation
class string
{
    struct string_data
    {
        uint32_t size;
        uint32_t ref_cnt;
        char data[0];

        static string_data* create(const char* s);
        static string_data* create(const char* s, uint32_t size);

        static string_data* create(uint32_t size);

        static string_data* createReverse(uint32_t cap);

        void inc_ref()
        {
            ref_cnt++;
        }

        void dec_ref()
        {
            if (--ref_cnt <= 0) {
                delete[] (char*)this;
            }
        }

    };

    string_data* ref;

    void inc_ref() {
        if (ref) ref->inc_ref();
    }

    void dec_ref() {
        if (ref) ref->dec_ref();
    }

public:
    string() : ref(nullptr) {}

    string(const char* s)
        : ref(nullptr)
    {
        if (!s) {
            return;
        }
        ref = string_data::create(s);
    }

    string(const char* s, uint32_t len)
        : ref(nullptr)
    {
        if (!s && len != 0) {
            return;
        }
        ref = string_data::create(s ? s : "", len);
    }

    string(std::string_view view)
        : ref(nullptr)
    {
        if (view.empty()) {
            return;
        }
        ref = string_data::create(view.data(), static_cast<uint32_t>(view.size()));
    }

    string(const std::string &value)
        : string(std::string_view(value))
    {}

    string(llvm::StringRef value)
        : string(std::string_view(value.data(), value.size()))
    {}

    string(const string& other)
    {
        ref = other.ref;
        inc_ref();
    }

    string(string&& other) noexcept
    {
        ref = other.ref;
        other.ref = nullptr;
    }

    string& operator=(const string& other)
    {
        if (this != &other) {
            dec_ref();
            ref = other.ref;
            inc_ref();
        }
        return *this;
    }

    string& operator=(string&& other) noexcept
    {
        if (this != &other) {
            dec_ref();
            ref = other.ref;
            other.ref = nullptr;
        }
        return *this;
    }

    string& operator=(const char* s)
    {
        dec_ref();
        if (!s || s[0] == '\0') {
            ref = nullptr;
            return *this;
        }
        ref = string_data::create(s);
        return *this;
    }

    string& operator=(const std::string &value)
    {
        dec_ref();
        if (value.empty()) {
            ref = nullptr;
            return *this;
        }
        ref = string_data::create(value.data(), static_cast<uint32_t>(value.size()));
        return *this;
    }

    string& operator=(llvm::StringRef value)
    {
        dec_ref();
        if (value.empty()) {
            ref = nullptr;
            return *this;
        }
        ref = string_data::create(value.data(), static_cast<uint32_t>(value.size()));
        return *this;
    }

    const char* tochara() const
    {
        return ref ? ref->data : "";
    }

    uint32_t size() const
    {
        return ref ? ref->size : 0;
    }

    bool empty() const
    {
        return size() == 0;
    }

    std::string_view view() const
    {
        return {tochara(), size()};
    }

    char operator[](size_t index) const
    {
        if (!ref || index >= ref->size) {
            throw "out of range";
        }
        return ref->data[index];
    }

    string& operator+=(const char* s)
    {
        if (!s || s[0] == '\0') {
            return *this;
        }

        uint32_t len = std::strlen(s);
        uint32_t old_size = size();
        uint32_t new_size = old_size + len;

        string_data* new_ref = string_data::create(new_size);

        if (ref) {
            std::memcpy(new_ref->data, ref->data, old_size);
            ref->dec_ref();
        }
        std::memcpy(new_ref->data + old_size, s, len + 1);
        ref = new_ref;

        return *this;
    }

    string& operator+=(std::string_view view)
    {
        if (view.empty()) {
            return *this;
        }

        uint32_t old_size = size();
        uint32_t new_size = old_size + static_cast<uint32_t>(view.size());
        string_data* new_ref = string_data::create(new_size);

        if (ref) {
            std::memcpy(new_ref->data, ref->data, old_size);
            ref->dec_ref();
        }
        std::memcpy(new_ref->data + old_size, view.data(), view.size());
        new_ref->data[new_size] = '\0';
        ref = new_ref;

        return *this;
    }

    string& operator+=(const std::string &value)
    {
        return (*this += std::string_view(value));
    }

    template<typename T>
    string& operator+=(T num)
    {
        // only support int32, int64, uint32, uint64, float, double
        if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, int64_t> ||
                       std::is_same_v<T, uint32_t> || std::is_same_v<T, uint64_t> ||
                       std::is_same_v<T, float> || std::is_same_v<T, double>) {
            // ok
        } else {
            throw "unsupported type for string concatenation";
        }
        constexpr int buffer_size = std::is_same_v<T, float> || std::is_same_v<T, double> ? 32 : 20;
        string_data* new_ref = string_data::createReverse(ref->size + buffer_size + 1);
        std::memcpy(new_ref->data, ref->data, ref->size);
        char* p = new_ref->data + ref->size;
        auto res = std::to_chars(p, p + buffer_size, num);
        if (res.ec != std::errc()) {
            panic("to_chars failed");
        }
        *res.ptr = '\0';
        new_ref->size = ref->size + (res.ptr - p);
        ref->dec_ref();
        ref = new_ref;
        return *this;
    }

    string& operator+=(const string& other)
    {
        if (other.size() == 0) {
            return *this;
        }

        uint32_t old_size = size();
        uint32_t new_size = old_size + other.size();
        string_data* new_ref = string_data::create(new_size);

        if (ref) {
            std::memcpy(new_ref->data, ref->data, old_size);
            ref->dec_ref();
        }
        std::memcpy(new_ref->data + old_size, other.tochara(), other.size());
        new_ref->data[new_size] = '\0';
        ref = new_ref;

        return *this;
    }

    template<typename T>
    string operator+(T other) const
    {
        string result = *this;
        result += other;
        return result;
    }

    int32_t toI32() const
    {
        return strtol(tochara(), nullptr, 10);
    }

    uint32_t toU32() const
    {
        return (uint32_t)strtoul(tochara(), nullptr, 10);
    }

    int64_t toI64() const
    {
        return strtoll(tochara(), nullptr, 10);
    }

    u_int64_t toU64() const
    {
        return (u_int64_t)strtoull(tochara(), nullptr, 10);
    }

    float toF32() const
    {
        return strtof(tochara(), nullptr);
    }

    double toF64() const
    {
        return strtod(tochara(), nullptr);
    }

    ~string()
    {
        if (ref) ref->dec_ref();
    }
};

extern std::ostream& operator<<(std::ostream& os, const string& str);

inline bool
operator==(const string &lhs, const string &rhs) {
    return lhs.view() == rhs.view();
}

inline bool
operator!=(const string &lhs, const string &rhs) {
    return !(lhs == rhs);
}

inline bool
operator==(const string &lhs, std::string_view rhs) {
    return lhs.view() == rhs;
}

inline bool
operator==(const string &lhs, const std::string &rhs) {
    return lhs.view() == std::string_view(rhs);
}

inline bool
operator==(std::string_view lhs, const string &rhs) {
    return lhs == rhs.view();
}

inline bool
operator==(const std::string &lhs, const string &rhs) {
    return std::string_view(lhs) == rhs.view();
}

inline bool
operator!=(const string &lhs, std::string_view rhs) {
    return !(lhs == rhs);
}

inline bool
operator!=(const string &lhs, const std::string &rhs) {
    return !(lhs == rhs);
}

inline bool
operator!=(std::string_view lhs, const string &rhs) {
    return !(lhs == rhs);
}

inline bool
operator!=(const std::string &lhs, const string &rhs) {
    return !(lhs == rhs);
}

inline bool
operator<(const string &lhs, const string &rhs) {
    return lhs.view() < rhs.view();
}

inline bool
operator<(const string &lhs, std::string_view rhs) {
    return lhs.view() < rhs;
}

inline bool
operator<(std::string_view lhs, const string &rhs) {
    return lhs < rhs.view();
}

inline string
operator+(std::string_view lhs, const string &rhs) {
    string result(lhs);
    result += rhs;
    return result;
}

inline string
operator+(const std::string &lhs, const string &rhs) {
    return std::string_view(lhs) + rhs;
}

inline string
operator+(const char *lhs, const string &rhs) {
    return std::string_view(lhs ? lhs : "") + rhs;
}

namespace lona {

inline std::string
toStdString(const ::string &value) {
    return {value.tochara(), value.size()};
}

inline llvm::StringRef
toStringRef(const ::string &value) {
    return {value.tochara(), value.size()};
}

}  // namespace lona

namespace std {

template<>
struct hash<::string> {
    std::size_t operator()(const ::string &value) const noexcept {
        return std::hash<std::string_view>{}(value.view());
    }
};

}  // namespace std
