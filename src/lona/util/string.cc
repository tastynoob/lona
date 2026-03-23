#include "string.hh"



#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <llvm-18/llvm/ADT/StringRef.h>
#include <sys/types.h>
#include <charconv>

#include "panic.hh"


string::string_data* string::string_data::create(const char* s)
{
    uint32_t len = std::strlen(s);
    string_data* sd = (string_data*)new char[sizeof(string_data) + len + 1];
    sd->size = len;
    sd->ref_cnt = 1;
    std::memcpy(sd->data, s, len + 1);
    return sd;
}

string::string_data* string::string_data::create(const char* s, uint32_t size)
{
    string_data* sd = (string_data*)new char[sizeof(string_data) + size + 1];
    sd->size = size;
    sd->ref_cnt = 1;
    if (size != 0) {
        std::memcpy(sd->data, s, size);
    }
    sd->data[size] = '\0';
    return sd;
}

string::string_data* string::string_data::create(uint32_t size)
{
    string_data* sd = (string_data*)new char[sizeof(string_data) + size + 1];
    sd->size = size;
    sd->ref_cnt = 1;
    sd->data[size] = '\0';
    return sd;
}

string::string_data* string::string_data::createReverse(uint32_t cap)
{
    string_data* sd = (string_data*)new char[sizeof(string_data) + cap + 1];
    sd->size = 0;
    sd->ref_cnt = 1;
    sd->data[cap] = '\0';
    return sd;
}

std::ostream& operator<<(std::ostream& os, const string& str) {
    os << str.tochara();
    return os;
}
