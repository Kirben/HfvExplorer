// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"
namespace Storage { class Job; struct Result; }
namespace Dmg {
// Replace a UDIF container with its bounded read-only logical disk.
bool Unwrap(std::unique_ptr<HfsPlus::Source>&);
enum class Output { CompressedDmg, RawImage };
class Password;
Storage::Result Convert(const std::wstring& source,const std::wstring& destination,Output,Storage::Job&,
                        const Password* password=nullptr);
}
