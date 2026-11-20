// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"

namespace SparseDisk {
bool Unwrap(std::unique_ptr<HfsPlus::Source>&);
std::unique_ptr<HfsPlus::Source> OpenBundle(const std::wstring& path);
// Accept the bundle's metadata file from a file-only picker.
std::wstring BundleFromPlist(const std::wstring& path);
}
