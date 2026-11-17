// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Independent epoch conversion using the format flags documented in Apple
// hfs_format.h and MacOSStubs.c; no Apple implementation code is incorporated.
#include "HfsPlus.h"
namespace HfsPlus { namespace Time {
constexpr uint64_t Epoch=2082844800ULL,Maximum=Epoch+UINT32_MAX;
inline uint64_t Decode(uint32_t raw,bool expanded){return uint64_t(raw)+(expanded?Epoch:0);}
inline uint32_t Encode(uint64_t value,bool expanded){if(expanded){if(value<Epoch||value>Maximum)throw Error("Expanded HFS+ dates must be between January 1970 and February 2106");return uint32_t(value-Epoch);}if(value>UINT32_MAX)throw Error("This HFS+ timestamp requires expanded timestamp format");return uint32_t(value);}
} }
