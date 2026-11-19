// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"

namespace Ndif {
// Recognize Disk Copy images by the bcem resource, including self-mounting images
// in MacBinary. The mounting program in a self-mounting image is never run.
bool Unwrap(std::unique_ptr<HfsPlus::Source>&);
}
