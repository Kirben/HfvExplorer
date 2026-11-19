// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"
namespace ReadOnlySources {
// In-memory registration only: these names never name Windows files/devices.
class Registration {
 std::string path_;
public:
 explicit Registration(std::shared_ptr<HfsPlus::Source>,bool writable=false);
 ~Registration();
 Registration(const Registration&)=delete;
 const std::string& Path()const{return path_;}
};
bool IsPath(const char*);
std::shared_ptr<HfsPlus::Source> Find(const char*);
}
