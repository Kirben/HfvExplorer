// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"

namespace SparseDisk {
bool Unwrap(std::unique_ptr<HfsPlus::Source>&);
}
