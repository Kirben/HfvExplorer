// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"
namespace Dmg {
// Replace a UDIF container with its bounded read-only logical disk.
bool Unwrap(std::unique_ptr<HfsPlus::Source>&);
}
