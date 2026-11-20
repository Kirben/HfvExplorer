// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"

namespace Storage {class Job;}
namespace Apfs {
std::string FormatLabel(const std::u16string&);
// The caller must supply a newly created zero-filled, unpublished image.
void Format(HfsPlus::Source&,const std::u16string&,bool caseSensitive,Storage::Job&);
}
