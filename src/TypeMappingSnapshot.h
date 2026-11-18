// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include <vector>
#include <utility>
namespace TypeMappings {
struct Snapshot {
    std::vector<std::pair<std::string, std::string>> hfsToWindows, windowsToHfs;
};
Snapshot Capture();
void Apply(const Snapshot &);
} // namespace TypeMappings
