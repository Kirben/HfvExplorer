// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include <string_view>

namespace Apfs {
std::string EncodeAttributeName(std::u16string_view name);
std::string EncodeCatalogName(std::u16string_view name);
std::u32string NormalizedName(std::string_view name,bool fold);
std::u32string NormalizedName(std::u16string_view name,bool fold);
}
