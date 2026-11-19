// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"
#include <string_view>

namespace Dmg {
class Password {
    std::array<uint8_t,4096> utf8_{};
    unsigned size_=0;
public:
    explicit Password(std::wstring_view);
    ~Password();
    Password(const Password&)=delete;
    Password& operator=(const Password&)=delete;
    const uint8_t* Data()const{return utf8_.data();}
    unsigned Size()const{return size_;}
};

struct PasswordRequired : HfsPlus::Error {
    bool retry;
    explicit PasswordRequired(bool retry);
};

// Decode the outer encryption layer first. The result can be a raw disk or a UDIF container.
bool Unlock(std::unique_ptr<HfsPlus::Source>&,const Password*);
}
